/*
MIT License

Copyright (c) 2018 Gera Kazakov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "Hap.h"

#include <thread>
#include <mutex>
#include <condition_variable>

#include <io.h>
#include <fcntl.h>
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "winsock2.h"

namespace Hap
{
	class TcpImpl : public Tcp
	{
	private:
		std::thread task;
		bool running = false;

		SOCKET server;
		SOCKET client[Hap::MaxHttpSessions + 1];
		Hap::sid_t sess[Hap::MaxHttpSessions + 1];

		void run()
		{
			Log("TcpImpl::Run - enter\n");

			struct sockaddr_in address;
			int addrlen = sizeof(address);

			while (running)
			{
				fd_set readfds;
				FD_ZERO(&readfds);

				FD_SET(server, &readfds);

				for (int i = 0; i < sizeofarr(client); i++)
				{
					SOCKET sd = client[i];
					if (sd > 0)
						FD_SET(sd, &readfds);
				}

				timeval to = { 1, 0 };
				int rc = select(0, &readfds, NULL, NULL, &to);
				if (rc < 0)
				{
					Log("select error %d\n", rc);
				}

				if (rc == 0)
				{
					// timeout, process events
					for (int i = 0; i < sizeofarr(client); i++)
					{
						SOCKET sd = client[i];
						if (sd == 0)
							continue;

						Hap::sid_t sid = sess[i];
						if (sid == Hap::sid_invalid)
							continue;

						_http->Poll(sid, [sd](Hap::sid_t sid, char* buf, uint16_t len) -> int
						{
							if (buf != nullptr)
								return send(sd, buf, len, 0);
							return 0;
						});
					}
				}

				// read event on server socket - incoming connection
				if (FD_ISSET(server, &readfds))
				{
					int clnt = accept(server, (struct sockaddr *)&address, &addrlen);
					if (clnt == INVALID_SOCKET)
					{
						Log("accept error\n");
					}
					else
					{
						Log("Connection on socket %d from ip %s  port %d\n", clnt,
							inet_ntoa(address.sin_addr), ntohs(address.sin_port));

						//add new socket to array of sockets 
						for (int i = 0; i < sizeofarr(client); i++)
						{
							if (client[i] == 0)
							{
								client[i] = clnt;
								break;
							}
						}
					}
				}

				// read event on client socket - data or disconnect
				for (int i = 0; i < sizeofarr(client); i++)
				{
					SOCKET sd = client[i];

					if (FD_ISSET(sd, &readfds))
					{
						bool close = false;
						Hap::sid_t sid = sess[i];
						
						if (sid == Hap::sid_invalid)
						{
							sid = _http->Open();
							sess[i] = sid;
						}

						if (sid == Hap::sid_invalid)
						{
							Log("Cannot open HTTP session for client %d\n", i);
							close = true;
						}
						else
						{
							bool rc = _http->Process(sid,
								[sd](Hap::sid_t sid, char* buf, uint16_t size) -> int
								{
									return recv(sd, buf, size, 0);
								},
								[sd](Hap::sid_t sid, char* buf, uint16_t len) -> int
								{
									if (buf != nullptr)
										return send(sd, buf, len, 0);
									return 0;
								}
							);

							if (!rc)
							{
								Log("HTTP Disconnect\n");
								close = true;
							}
						}

						if (close)
						{
							getpeername(sd, (struct sockaddr*)&address, &addrlen);
							Log("Disconnect socket %d to ip %s  port %d\n", sd,
								inet_ntoa(address.sin_addr), ntohs(address.sin_port));

							closesocket(sd);
							client[i] = 0;
							_http->Close(sid);
							sess[i] = sid_invalid;
						}
#if 0
						char buf[512];
						// data is available on session sd

						//Check if it was for closing , and also read the incoming message 
						int rc = recv(sd, buf, sizeof(buf), 0);
						if (rc <= 0)
						{
							//Somebody disconnected , get his details and print 
							getpeername(sd, (struct sockaddr*)&address, &addrlen);
							printf("Host disconnected , ip %s , port %d \n",
								inet_ntoa(address.sin_addr), ntohs(address.sin_port));

							//Close the socket and mark as 0 in list for reuse 
							closesocket(sd);
							client[i] = 0;
						}

						//Echo back the message that came in 
						else
						{
							//set the string terminating NULL byte on the end of the data read 
							buf[rc++] = '\r';
							buf[rc++] = '\n';
							buf[rc++] = '\r';
							buf[rc++] = '\n';
							send(sd, buf, rc, 0);
						}
#endif
					}
				}
			}

			for (int i = 0; i < sizeofarr(client); i++)
			{
				SOCKET sd = client[i];
				if (sd != 0)
				{
					closesocket(sd);
					client[i] = 0;
				}
			}

			Log("TcpImpl::Run - exit\n");
		}

	public:
		TcpImpl()
		{
			WSADATA wsaData = { 0 };
			WSAStartup(MAKEWORD(2, 2), &wsaData);
		}

		~TcpImpl()
		{
			Stop();
			WSACleanup();
		}

		virtual bool Start() override
		{
			for (int i = 0; i < sizeofarr(client); i++)
			{
				client[i] = 0;
				sess[i] = sid_invalid;
			}

			//create the server socket 
			server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (server == INVALID_SOCKET)
			{
				Log("server socket creation failed");
				return false;
			}

			// allow llocal address reuse
			BOOL opt = 1;
			if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0)
			{
				Log("setsockopt(server, SO_REUSEADDR) failed");
				return false;
			}

			//bind the socket
			struct sockaddr_in address;
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = INADDR_ANY;
			address.sin_port = Hap::config->port;
			if (bind(server, (struct sockaddr *)&address, sizeof(address))<0)
			{
				Log("bind(server, INADDR_ANY) failed");
				return false;
			}

			if (listen(server, MaxHttpSessions) < 0)
			{
				Log("listen(server) failed");
				return false;
			}

			running = true;
			task = std::thread(&TcpImpl::run, this);

			return running;
		}

		virtual void Stop() override
		{
			running = false;

			// closesocket causes select to return then accept fails - works on windows, not on linux
			closesocket(server);

			if (task.joinable())
				task.join();
		}

	} tcp;

	Tcp* Tcp::Create(Hap::Http::Server* http)
	{
		tcp._http = http;
		return &tcp;
	}
}