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

#include "srp/srp.h"

namespace Hap
{
	namespace Http
	{

		const char* ContentTypeJson = "application/hap+json";
		const char* ContentTypeTlv8 = "application/pairing+tlv8";

		// current pairing session - only one simultaneous pairing is allowed
		SRP* srp = NULL;					// !NULL = pairing in progress, only one pairing at a time
		uint8_t srp_shared_secret[64];		// SRP shared secret
		sid_t srp_owner = sid_invalid;		// session owning the srp
		uint8_t srp_auth_count = 0;			// auth attempts counter

		// Open
		//	returns new session ID, 0..sid_max, or sid_invalid
		sid_t Server::Open()
		{
			for (sid_t sid = 0; sid < sizeofarr(_sess); sid++)
			{
				if (_sess[sid].isOpen())
					continue;

				// open session - use same buffers for all sessions
				_sess[sid].Open(sid, &_buf);

				// open database
				_db.Open(sid);

				return sid;
			}

			return sid_invalid;
		}

		// Close
		//	returns true if opened session was closed
		bool Server::Close(sid_t sid)
		{
			if (sid > sid_max)
				return false;

			if (!_sess[sid].isOpen())
				return false;

			_db.Close(sid);

			_sess[sid].Close();

			// cancel current pairing if any
			if (srp != NULL && srp_owner == sid)
			{
				SRP_free(srp);
				srp = NULL;
				srp_owner = sid_invalid;
			}

			return true;
		}

		bool Server::Process(sid_t sid, Recv recv, Send send)
		{
			if (sid > MaxHttpSessions)	// invalid sid
				return false;

			Session* sess = &_sess[sid];
			bool secured = sess->secured;

			Log("Http::Process Ses %d  secured %d  %s\n", sid, sess->secured, sess->ios ? (sess->ios->perm == Hap::Controller::Admin ? "admin" : "user") : "?");

			if (sid == MaxHttpSessions)	// too many sessions
			{
				// TODO: read request, create error response
				send(sid, sess->rsp.buf(), sess->rsp.len());
				return false;
			}

			// prepare for request parsing
			sess->Init();

			// read and parse the HTTP request
			uint16_t len = 0;		// total len of valid data received so far
			uint16_t http_len;		// length of http request
			while (true)
			{
				uint8_t* req = sess->data() + len;
				uint16_t req_len = sess->sizeofdata() - len;
				
				// read next portion of the request
				int l = recv(sid, (char*)req, req_len);
				if (l < 0)	// read error
				{
					Log("Http: Read Error\n");
					return false;
				}
				if (l == 0)
				{
					Log("Http: Read EOF\n");
					return false;
				}

				len += l;

				if (sess->secured)
				{
					// it session is secured, decrypt the block - when the whole block is received
					//	max length of http data that can be processed is defined by MaxHttpFrame/MaxHttpBlock

					if (len < 2)	// wait fot at least two bytes of data length 
						continue;
					
					uint8_t *p = sess->data();
					uint16_t aad = p[0] + ((uint16_t)(p[1]) << 8);	// data length, also serves as AAD for decryption

					if (aad > MaxHttpBlock)
					{
						Log("Http: encrypted block size is too big: %d\n", aad);
						return false;
					}

					if (len < 2 + aad + 16)	// wait for complete encrypted block
						continue;

					// make 96-bit nonce from receive sequential number
					uint8_t nonce[12];
					memset(nonce, 0, sizeof(nonce));
					memcpy(nonce + 4, &sess->recvSeq, 8);

					// decrypt into request buffer
					uint8_t* b = (uint8_t*)sess->req.buf();

					Hap::Crypt::aead(Hap::Crypt::Decrypt,
						b, b + aad,							// output data and tag positions
						sess->ControllerToAccessoryKey,		// decryption key
						nonce,
						p + 2, aad,							// encrypted data
						p, 2								// aad
					);

					sess->recvSeq++;

					// compare passed in and calculated tags
					if (memcmp(b + aad, p + 2 + aad, 16) != 0)
					{
						Log("Http: decrypt error\n");
						return false;
					}

					http_len = aad;
				}
				else
				{
					// otherwise copy received data into request buffer as is
					memcpy(sess->req.buf(), sess->data(), len);

					http_len = len;
				}

				// try parsing HTTP request
				auto status = sess->req.parse(http_len);
				if (status == sess->req.Error)	// parser error
				{
					// TODO: make response Internal server error
					send(sid, sess->rsp.buf(), sess->rsp.len());
					return false;
				}

				if (status == sess->req.Success)
					// request parsed, stop reading 
					break;

				if (sess->secured)
				{
					// if session is sequred then whole request must fit into single frame
					// TODO: support multiple frames
					Log("Http: request doesn not fit into single frame\n");
					return false;
				}

				// request incomplete - try reading more data
			}

			auto m = sess->req.method();
			Log("Method: '%.*s'\n", m.len(), m.ptr());

			auto p = sess->req.path();
			Log("Path: '%.*s'\n", p.len(), p.ptr());

			auto d = sess->req.data();

			for (size_t i = 0; i < sess->req.hdr_count(); i++)
			{
				auto n = sess->req.hdr_name(i);
				auto v = sess->req.hdr_value(i);
				Log("%.*s: '%.*s'\n", n.len(), n.ptr(), v.len(), v.ptr());
			}

			if (m.len() == 4 && strncmp(m.ptr(), "POST", 4) == 0)
			{
				// POST
				//		/identify
				//		/pair-setup
				//		/pair-verify
				//		/pairings

				if (p.len() == 9 && strncmp(p.ptr(), "/identify", 9) == 0)
				{
					if (_pairings.Count() == 0)
					{
						Log("Http: Exec unpaired identify\n");
						sess->rsp.start(HTTP_204);
						sess->rsp.end();
					}
					else
					{
						Log("Http: Unpaired identify prohibited when paired\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.add(ContentType, ContentTypeJson);
						sess->rsp.end("{\"status\":-70401}");
					}
				}
				else if (p.len() == 11 && strncmp(p.ptr(), "/pair-setup", 11) == 0)
				{
					int len;
					if (!sess->req.hdr(ContentType, ContentTypeTlv8))
					{
						Log("Http: Unknown or missing ContentType\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else if (!sess->req.hdr(ContentLength, len))
					{
						Log("Http: Unknown or missing ContentLength\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else
					{
						sess->tlvi.parse(d.ptr(), d.len());
						Log("PairSetup: TLV item count %d\n", sess->tlvi.count());

						Tlv::State state;
						if (!sess->tlvi.get(Tlv::Type::State, state))
						{
							Log("PairSetup: State not found\n");
						}
						else
						{
							switch (state)
							{
							case Tlv::State::M1:
								_pairSetup1(sess);
								break;

							case Tlv::State::M3:
								_pairSetup3(sess);
								break;

							case Tlv::State::M5:
								_pairSetup5(sess);
								break;

							default:
								Log("PairSetup: Unknown state %d\n", (int)state);
							}
						}
					}
				}
				else if (p.len() == 12 && strncmp(p.ptr(), "/pair-verify", 12) == 0)
				{
					int len;
					if (!sess->req.hdr(ContentType, ContentTypeTlv8))
					{
						Log("Http: Unknown or missing ContentType\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else if (!sess->req.hdr(ContentLength, len))
					{
						Log("Http: Unknown or missing ContentLength\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else
					{
						sess->tlvi.parse(d.ptr(), d.len());
						Log("PairVerify: TLV item count %d\n", sess->tlvi.count());

						Tlv::State state;
						if (!sess->tlvi.get(Tlv::Type::State, state))
						{
							Log("PairVerify: State not found\n");
						}
						else
						{
							switch (state)
							{
							case Tlv::State::M1:
								_pairVerify1(sess);
								break;

							case Tlv::State::M3:
								_pairVerify3(sess);
								secured = sess->ios != nullptr;
								break;

							default:
								Log("PairVerify: Unknown state %d\n", (int)state);
							}
						}
					}
				}
				else if (p.len() == 9 && strncmp(p.ptr(), "/pairings", 9) == 0)
				{
					int len;
					if (!sess->secured)
					{
						Log("Http: Authorization required\n");
						sess->rsp.start(HTTP_470);
						sess->rsp.end();
					}
					else if (!sess->req.hdr(ContentType, ContentTypeTlv8))
					{
						Log("Http: Unknown or missing ContentType\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else if (!sess->req.hdr(ContentLength, len))
					{
						Log("Http: Unknown or missing ContentLength\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else
					{
						sess->tlvi.parse(d.ptr(), d.len());
						Log("Pairings: TLV item count %d\n", sess->tlvi.count());

						Tlv::State state;
						if (!sess->tlvi.get(Tlv::Type::State, state))
						{
							Log("Pairings: State not found\n");
							sess->rsp.start(HTTP_400);
							sess->rsp.end();
						}
						else
						{
							if(state != Tlv::State::M1)
							{
								Log("Pairings: Invalid State\n");
								sess->rsp.start(HTTP_400);
								sess->rsp.end();
							}
							else
							{
								Tlv::Method method;
								if (!sess->tlvi.get(Tlv::Type::Method, method))
								{
									Log("Pairings: Method not found\n");
									sess->rsp.start(HTTP_400);
									sess->rsp.end();
								}
								else
								{
									switch (method)
									{
									case Tlv::Method::AddPairing:
										_pairingAdd(sess);
										break;

									case Tlv::Method::RemovePairing:
										_pairingRemove(sess);
										break;

									case Tlv::Method::ListPairing:
										_pairingList(sess);
										break;

									default:
										Log("Pairings: Unknown method\n");
										sess->rsp.start(HTTP_400);
										sess->rsp.end();
									}
								}
							}
						}
					}
				}
				else
				{
					Log("Http: Unknown path %.*s\n", p.len(), p.ptr());
					sess->rsp.start(HTTP_400);
					sess->rsp.end();
				}
			}
			else if (m.len() == 3 && strncmp(m.ptr(), "GET", 3) == 0)
			{
				// GET
				//		/accessories
				//		/characteristics
				if (!sess->secured)
				{
					Log("Http: Authorization required\n");
					sess->rsp.start(HTTP_470);
					sess->rsp.end();
				}
				else if (p.len() == 12 && strncmp(p.ptr(), "/accessories", 12) == 0)
				{
					sess->rsp.start(HTTP_200);
					sess->rsp.add(ContentType, ContentTypeJson);
					sess->rsp.add(ContentLength, 0);
					sess->rsp.end();

					int len = _db.getDb(sess->Sid(), sess->rsp.data(), sess->rsp.size());

					Log("Db: '%.*s'\n", len, sess->rsp.data());

					sess->rsp.setContentLength(len);
				}
				else if(strncmp(p.ptr(), "/characteristics?", 17) == 0)
				{

					int len = sess->sizeofdata();
					auto status = _db.Read(sess->Sid(), p.ptr() + 17, p.len() - 17, (char*)sess->data(), len);

					Log("Read: Status %d  '%.*s'\n", status, len, sess->data());

					sess->rsp.start(status);
					if (len > 0)
					{
						sess->rsp.add(ContentType, ContentTypeJson);
						sess->rsp.end((const char*)sess->data(), len);
					}
					else
					{
						sess->rsp.end();
					}
				}
				else
				{
					Log("Http: Unknown path %.*s\n", p.len(), p.ptr());
					sess->rsp.start(HTTP_400);
					sess->rsp.end();
				}
			}
			else if (m.len() == 3 && strncmp(m.ptr(), "PUT", 3) == 0)
			{
				// PUT
				//		/characteristics
				if (p.len() == 16 && strncmp(p.ptr(), "/characteristics", 16) == 0)
				{
					int len;
					if (!sess->secured)
					{
						Log("Http: Authorization required\n");
						sess->rsp.start(HTTP_470);
						sess->rsp.end();
					}
					else if (!sess->req.hdr(ContentType, ContentTypeJson))
					{
						Log("Http: Unknown or missing ContentType\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else if (!sess->req.hdr(ContentLength, len))
					{
						Log("Http: Unknown or missing ContentLength\n");
						sess->rsp.start(HTTP_400);
						sess->rsp.end();
					}
					else
					{
						Log("Http: %.*s\n", d.len(), d.ptr());

						int len = sess->sizeofdata();
						auto status = _db.Write(sess->Sid(), (const char*)d.ptr(), d.len(), (char*)sess->data(), len);

						Log("Write: Status %d  '%.*s'\n", status, len, sess->data());

						sess->rsp.start(status);
						if (len > 0)
						{
							sess->rsp.add(ContentType, ContentTypeJson);
							sess->rsp.end((const char*)sess->data(), len);
						}
						else
						{
							sess->rsp.end();
						}
					}
				}
				else
				{
					Log("Http: Unknown path %.*s\n", p.len(), p.ptr());
					sess->rsp.start(HTTP_400);
					sess->rsp.end();
				}
			}

			if (!_send(sess, send))
				return false;

			sess->secured = secured;
			Log("Http::Process exit Ses %d  secured %d\n", sid, sess->secured);

			return true;
		}

		void Server::Poll(sid_t sid, Send send)
		{
			Session* sess = &_sess[sid];
			if (!sess->secured)
				return;

			int len = sess->sizeofdata();
			auto status = _db.getEvents(sid, (char*)sess->data(), len);

			if (status != Http::Status::HTTP_200)
				return;

			if (len == 0)
				return;

			Log("Events: sid %d  '%.*s'\n", sid, len, sess->data());

			sess->rsp.event(status);
			sess->rsp.add(ContentType, ContentTypeJson);
			sess->rsp.end((const char*)sess->data(), len);

			_send(sess, send);
		}

		bool Server::_send(Session* sess, Send& send)
		{
			if (sess->secured)
			{
				// session secured - encrypt data
				const uint8_t *p = (uint8_t*)sess->rsp.buf();
				uint16_t len = sess->rsp.len();				// data length
				
				while (len > 0)
				{
					uint16_t aad = len;		// block length, and AAD for encryption

					if (aad > MaxHttpBlock)
						aad = MaxHttpBlock;

					// make 96-bit nonce from send sequential number
					uint8_t nonce[12];
					memset(nonce, 0, sizeof(nonce));
					memcpy(nonce + 4, &sess->sendSeq, 8);

					// encrypt into sess->data buffer which must be >= MaxHttpFrame
					uint8_t* b = sess->data();

					// copy data length into output buffer
					b[0] = aad & 0xFF;
					b[1] = (aad >> 8) & 0xFF;

					Hap::Crypt::aead(Hap::Crypt::Encrypt,
						b + 2, b + 2 + aad,					// output data and tag positions
						sess->AccessoryToControllerKey,		// encryption key
						nonce,
						p, aad,								// data to encrypt
						b, 2								// aad
					);

					sess->sendSeq++;

					// send encrypted block
					send(sess->Sid(), (char*)b, 2 + aad + 16);

					len -= aad;
					p += aad;
				}
			}
			else
			{
				//send response as is
				send(sess->Sid(), sess->rsp.buf(), sess->rsp.len());
			}

			return true;
		}


		void Server::_pairSetup1(Session* sess)
		{
			int rc;
			cstr* pub = NULL;

			Log("PairSetupM1\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M2);

			// verify that valid Method is present in input TLV
			Tlv::Method method;
			if (!sess->tlvi.get(Tlv::Type::Method, method))
			{
				Log("PairSetupM1: Method not found\n");
				goto RetErr;
			}
			if (method != Tlv::Method::PairSetupNonMfi)
			{
				Log("PairSetupM1: Invalid Method\n");
				goto RetErr;
			}

			// if the accessory is already paired it must respond Error_Unavailable
			if (_pairings.Count() != 0)
			{
				Log("PairSetupM1: Already paired, return Error_Unavailable\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unavailable);
				goto Ret;
			}

			// if accessory received more than 100 unsuccessfull auth attempts, respond Error_MaxTries
			if (srp_auth_count > 100)
			{
				Log("PairSetupM1: Too many auth attempts, return Error_MaxTries\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::MaxTries);
				goto Ret;
			}

			// if accessory is currently performing PairSetup with different controller, respond Error_Busy
			if (srp != NULL && srp_owner != sess->Sid())
			{
				Log("PairSetupM1: Already pairing, return Error_Busy\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Busy);
				goto Ret;
			}
			
			// create new pairing session
			srp = SRP_new(SRP6a_server_method());
			if (srp == NULL)
			{
				Log("PairSetupM1: SRP_new error\n");
				goto RetErr;
			}

			srp_owner = sess->Sid();
			srp_auth_count++;

			rc = SRP_set_username(srp, "Pair-Setup");
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_set_username error %d\n", rc);
				goto RetErr;
			}

			Hex("Username", srp->username->data, srp->username->length);

			uint8_t salt[16];
			t_random(salt, 16);
			rc = SRP_set_params(srp,
				srp_modulus, sizeof_srp_modulus,
				srp_generator, sizeof_srp_generator,
				salt, sizeof(salt)
			);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_set_params error %d\n", rc);
				goto RetErr;
			}

			Hex("Modulus", srp_modulus, sizeof_srp_modulus);
			Hex("Generator", srp_generator, sizeof_srp_generator);
			Hex("Salt", salt, sizeof(salt));

			rc = SRP_set_auth_password(srp, Hap::config->setupCode);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_set_auth_password error %d\n", rc);
				goto RetErr;
			}

			Hex("Username", Hap::config->setupCode, strlen(Hap::config->setupCode));

			rc = SRP_gen_pub(srp, &pub);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM1: SRP_gen_pub error %d\n", rc);
				goto RetErr;
			}

			Hex("ServerKey", pub->data, pub->length);

			sess->tlvo.add(Hap::Tlv::Type::PublicKey, pub->data, (uint16_t)pub->length);
			sess->tlvo.add(Hap::Tlv::Type::Salt, salt, sizeof(salt));

			goto Ret;

		RetErr:
			if (srp)
				SRP_free(srp);
			srp = NULL;
			srp_owner = sid_invalid;
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);
		
		Ret:
			if(pub != NULL)
				cstr_free(pub);

			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}
	
		void Server::_pairSetup3(Session* sess)
		{
			int rc;
			uint16_t size;
			uint8_t* iosKey = sess->data();
			uint8_t* iosProof = iosKey + 384;
			uint16_t iosKey_size = 384;
			uint16_t iosProof_size = 64;
			cstr* key = NULL;
			cstr* rsp = NULL;

			Log("PairSetupM3\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M4);

			// verify that pairing is in progress on current session
			if (srp == nullptr || srp_owner != sess->Sid())
			{
				Log("PairSetupM3: No active pairing\n");
				goto RetErr;
			}

			// verify that required items are present in input TLV
			size = iosKey_size;
			if (!sess->tlvi.get(Tlv::Type::PublicKey, iosKey, iosKey_size))
			{
				Log("PairSetupM3: PublicKey not found\n");
				goto RetErr;
			}

			Hex("iosKey", iosKey, iosKey_size);

			size = iosProof_size;
			if (!sess->tlvi.get(Tlv::Type::Proof, iosProof, iosProof_size))
			{
				Log("PairSetupM3: Proof not found\n");
				goto RetErr;
			}

			Hex("iosProof", iosProof, iosProof_size);

			rc = SRP_compute_key(srp, &key, iosKey, iosKey_size);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM3: SRP_compute_key error %d\n", rc);
				goto RetErr;
			}

			memcpy(srp_shared_secret, key->data, key->length);

			Hap::Crypt::hkdf(
				(const uint8_t*)"Pair-Setup-Encrypt-Salt", sizeof("Pair-Setup-Encrypt-Salt") - 1,
				srp_shared_secret, sizeof(srp_shared_secret),
				(const uint8_t*)"Pair-Setup-Encrypt-Info", sizeof("Pair-Setup-Encrypt-Info") - 1,
				sess->key, sizeof(sess->key));

			Hex("SessKey", sess->key, sizeof(sess->key));

			rc = SRP_verify(srp, iosProof, size);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM3: SRP_verify error %d\n", rc);
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
				goto Ret;
			}

			rc = SRP_respond(srp, &rsp);
			if (rc != SRP_SUCCESS)
			{
				Log("PairSetupM3: SRP_respond error %d\n", rc);
				goto RetErr;
			}

			Hex("Response", rsp->data, rsp->length);

			sess->tlvo.add(Hap::Tlv::Type::Proof, rsp->data, (uint16_t)rsp->length);

			goto Ret;

		RetErr:	// error, cancel current pairing, if this session owns it
			if (srp && srp_owner == sess->Sid())
			{
				SRP_free(srp);
				srp = NULL;
				srp_owner = sid_invalid;
			}
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:
			if (key != NULL)
				cstr_free(key);
			if (rsp != NULL)
				cstr_free(rsp);

			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}

		void Server::_pairSetup5(Session* sess)
		{
			uint8_t* iosEncrypted;	// encrypted tata from iOS with tag attached
			uint8_t* iosTag;		// pointer to iOS tag
			uint8_t* iosTlv;		// decrypted TLV
			uint8_t* srvTag;		// calculated tag
			uint16_t iosTlv_size;

			Log("PairSetupM5\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M6);

			// verify that pairing is in progress on current session
			if (srp == nullptr || srp_owner != sess->Sid())
			{
				Log("PairSetupM5: No active pairing\n");
				goto RetErr;
			}

			// extract encrypted data into sess->data buffer
			iosEncrypted = sess->data();
			iosTlv_size = sess->sizeofdata();
			if (!sess->tlvi.get(Tlv::Type::EncryptedData, iosEncrypted, iosTlv_size))
			{
				Log("PairSetupM5: EncryptedData not found\n");
				goto RetErr;
			}
			else
			{
				Hap::Tlv::Item id;
				Hap::Tlv::Item ltpk;
				Hap::Tlv::Item sign;

				// format sess->data buffer
				iosTlv = iosEncrypted + iosTlv_size;	// decrypted TLV
				iosTlv_size -= 16;						// strip off tag
				iosTag = iosEncrypted + iosTlv_size;	// iOS tag location
				srvTag = iosTlv + iosTlv_size;			// place for our tag

				// decrypt iOS data using session key
				Hap::Crypt::aead(Hap::Crypt::Decrypt, iosTlv, srvTag,
					sess->key, (const uint8_t *)"\x00\x00\x00\x00PS-Msg05", 
					iosEncrypted, iosTlv_size);

				Hex("iosTlv", iosTlv, iosTlv_size);
				Hex("iosTag", iosTag, 16);
				Hex("srvTlv", srvTag, 16);

				// compare calculated tag with passed in one
				if (memcmp(iosTag, srvTag, 16) != 0)
				{
					Log("PairSetupM5: authTag does not match\n");
					sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
					goto Ret;
				}

				// parse decrypted TLV - 3 items expected
				Hap::Tlv::Parse<3> tlv(iosTlv, iosTlv_size);
				Log("PairSetupM5: TLV item count %d\n", tlv.count());

				// extract TLV items
				if (!tlv.get(Hap::Tlv::Type::Identifier, id))
				{
					Log("PairSetupM5: Identifier not found\n");
					goto RetErr;
				}
				Hex("iosPairingId:", id.val(), id.len());

				if (!tlv.get(Hap::Tlv::Type::PublicKey, ltpk))
				{
					Log("PairSetupM5: PublicKey not found\n");
					goto RetErr;
				}
				Hex("iosLTPK:", ltpk.val(), ltpk.len());

				if (!tlv.get(Hap::Tlv::Type::Signature, sign))
				{
					Log("PairSetupM5: Signature not found\n");
					goto RetErr;
				}
				Hex("iosSignature:", sign.val(), sign.len());

				// TODO: build iOSDeviceInfo and verify iOS device signature

				// add pairing info to pairig database
				if (!_pairings.Add(id, ltpk, Controller::Admin))
				{
					Log("PairSetupM5: cannot add Pairing record\n");
					sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::MaxPeers);
					goto Ret;
				}

				// buid Accessory Info and sign it
				uint8_t* AccesoryInfo = sess->data();	// re-use sess->data buffer
				uint8_t* p = AccesoryInfo;
				int l = sess->sizeofdata();

				// add AccessoryX
				Hap::Crypt::hkdf(
					(const uint8_t*)"Pair-Setup-Accessory-Sign-Salt", sizeof("Pair-Setup-Accessory-Sign-Salt") - 1,
					srp_shared_secret, sizeof(srp_shared_secret),
					(const uint8_t*)"Pair-Setup-Accessory-Sign-Info", sizeof("Pair-Setup-Accessory-Sign-Info") - 1,
					p, 32);
				p += 32;
				l -= 32;
				
				// add Accessory PairingId
				memcpy(p, config->deviceId, strlen(config->deviceId));
				p += strlen(config->deviceId);
				l -= strlen(config->deviceId);

				// add Accessory LTPK
				memcpy(p, _keys.PubKey(), _keys.PubKeySize);
				p += _keys.PubKeySize;
				l -= _keys.PubKeySize;

				// sign the info
				_keys.Sign(p, AccesoryInfo, p - AccesoryInfo);
				p += _keys.SignSize;
				l -= _keys.SignSize;

				// construct the sub-TLV
				Hap::Tlv::Create subTlv;
				subTlv.create(p, l);
				subTlv.add(Hap::Tlv::Type::Identifier, (const uint8_t*)config->deviceId, (uint16_t)strlen(config->deviceId));
				subTlv.add(Hap::Tlv::Type::PublicKey, _keys.PubKey(), _keys.PubKeySize);
				subTlv.add(Hap::Tlv::Type::Signature, p - _keys.SignSize, _keys.SignSize);
				p += subTlv.length();
				l -= subTlv.length();

				// enrypt AccessoryInfo using session key
				Hap::Crypt::aead(Hap::Crypt::Encrypt, 
					p,									// output encrypted TLV 
					p + subTlv.length(),				// output tag follows the encrypted TLV
					sess->key,						
					(const uint8_t *)"\x00\x00\x00\x00PS-Msg06",
					p - subTlv.length(),				// input TLV
					subTlv.length()						// TLV length
				);

				l -= subTlv.length() + 16;
				Log("PairSetupM5: sess->data unused: %d\n", l);

				// add encryped info and tag to output TLV
				sess->tlvo.add(Hap::Tlv::Type::EncryptedData, p, subTlv.length() + 16);

				Hap::config->Update();

				goto RetDone;	// pairing complete
			}

		RetErr:	// error, cancel current pairing, if this session owns it
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		RetDone:
			if (srp && srp_owner == sess->Sid())
			{
				SRP_free(srp);
				srp = NULL;
				srp_owner = sid_invalid;
			}

		Ret:
			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}

		void Server::_pairVerify1(Session* sess)
		{
			Hap::Tlv::Item iosKey;
			const uint8_t* sharedSecret;
			uint8_t* p;
			int l;

			Log("PairVerifyM1\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M2);

			// verify that PublicKey is present in input TLV
			if (!sess->tlvi.get(Tlv::Type::PublicKey, iosKey))
			{
				Log("PairVerifyM1: PublicKey not found\n");
				goto RetErr;
			}

			// create new Curve25519 key pair
			sess->curve.Init();

			// generate shared secret
			sharedSecret = sess->curve.getSharedSecret(iosKey.val());

			// create session key from shared secret
			Hap::Crypt::hkdf(
				(const uint8_t*)"Pair-Verify-Encrypt-Salt", sizeof("Pair-Verify-Encrypt-Salt") - 1,
				sharedSecret, sess->curve.KeySize,
				(const uint8_t*)"Pair-Verify-Encrypt-Info", sizeof("Pair-Verify-Encrypt-Info") - 1,
				sess->key, sizeof(sess->key));

			// construct AccessoryInfo
			p = sess->data();
			l = sess->sizeofdata();

			//	add Curve25519 public key
			memcpy(p, sess->curve.getPublicKey(), sess->curve.KeySize);
			p += sess->curve.KeySize;
			l -= sess->curve.KeySize;

			//	add Accessory PairingId
			memcpy(p, config->deviceId, strlen(config->deviceId));
			p += strlen(config->deviceId);
			l -= strlen(config->deviceId);

			// add iOS device public key
			memcpy(p, iosKey.val(), iosKey.len());
			p += iosKey.len();
			l -= iosKey.len();

			// sign the AccessoryInfo
			_keys.Sign(p, sess->data(), p - sess->data());
			p += _keys.SignSize;
			l -= _keys.SignSize;

			// make sub-TLV
			Hap::Tlv::Create subTlv;
			subTlv.create(p, l);
			subTlv.add(Hap::Tlv::Type::Identifier, (const uint8_t*)config->deviceId, (uint16_t)strlen(config->deviceId));
			subTlv.add(Hap::Tlv::Type::Signature, p - _keys.SignSize, _keys.SignSize);
			p += subTlv.length();
			l -= subTlv.length();

			// encrypt sub-TLV using session key
			Hap::Crypt::aead(Hap::Crypt::Encrypt,
				p,									// output encrypted TLV 
				p + subTlv.length(),				// output tag follows the encrypted TLV
				sess->key,
				(const uint8_t *)"\x00\x00\x00\x00PV-Msg02",
				p - subTlv.length(),				// input TLV
				subTlv.length()						// TLV length
			);

			l -= subTlv.length() + 16;
			Log("PairVerifyM1: sess->data unused: %d\n", l);

			// add Accessory public key to output TLV
			sess->tlvo.add(Hap::Tlv::Type::PublicKey, sess->curve.getPublicKey(), sess->curve.KeySize);

			// add encryped info and tag to output TLV
			sess->tlvo.add(Hap::Tlv::Type::EncryptedData, p, subTlv.length() + 16);

			goto Ret;

		RetErr:	// error
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:
			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}

		void Server::_pairVerify3(Session* sess)
		{
			uint8_t* iosEncrypted;	// encrypted tata from iOS with tag attached
			uint8_t* iosTag;		// pointer to iOS tag
			uint8_t* iosTlv;		// decrypted TLV
			uint8_t* srvTag;		// calculated tag
			uint16_t iosTlv_size;

			Log("PairVerifyM3\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M4);

			// extract encrypted data into sess->data buffer
			iosEncrypted = sess->data();
			iosTlv_size = sess->sizeofdata();
			if (!sess->tlvi.get(Tlv::Type::EncryptedData, iosEncrypted, iosTlv_size))
			{
				Log("PairVerifyM3: EncryptedData not found\n");
				goto RetErr;
			}
			else
			{
				Hap::Tlv::Item id;
				Hap::Tlv::Item sign;

				// format sess->data buffer
				iosTlv = iosEncrypted + iosTlv_size;	// decrypted TLV
				iosTlv_size -= 16;						// strip off tag
				iosTag = iosEncrypted + iosTlv_size;	// iOS tag location
				srvTag = iosTlv + iosTlv_size;			// place for our tag

														// decrypt iOS data using session key
				Hap::Crypt::aead(Hap::Crypt::Decrypt, iosTlv, srvTag,
					sess->key, (const uint8_t *)"\x00\x00\x00\x00PV-Msg03",
					iosEncrypted, iosTlv_size);

				Hex("iosTlv", iosTlv, iosTlv_size);
				Hex("iosTag", iosTag, 16);
				Hex("srvTlv", srvTag, 16);

				// compare calculated tag with passed in one
				if (memcmp(iosTag, srvTag, 16) != 0)
				{
					Log("PairVerifyM3: authTag does not match\n");
					sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
					goto Ret;
				}

				// parse decrypted TLV - 2 items expected
				Hap::Tlv::Parse<2> tlv(iosTlv, iosTlv_size);
				Log("PairVerifyM3: TLV item count %d\n", tlv.count());

				// extract TLV items
				if (!tlv.get(Hap::Tlv::Type::Identifier, id))
				{
					Log("PairVerifyM3: Identifier not found\n");
					goto RetErr;
				}
				Hex("iosPairingId:", id.val(), id.len());

				if (!tlv.get(Hap::Tlv::Type::Signature, sign))
				{
					Log("PairVerifyM3: Signature not found\n");
					goto RetErr;
				}
				Hex("iosSignature:", sign.val(), sign.len());

				// lookup iOS id in pairing database
				auto ios = _pairings.Get(id);
				if (ios == nullptr)
				{
					Log("PairVerifyM3: iOS device ID not found\n");
					sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
					goto Ret;
				}

				// TODO: construct iOSDeviceInfo and verify signature

				// create session encryption keys
				Hap::Crypt::hkdf(
					(const uint8_t*)"Control-Salt", sizeof("Control-Salt") - 1,
					sess->curve.getSharedSecret(), sess->curve.KeySize,
					(const uint8_t*)"Control-Read-Encryption-Key", sizeof("Control-Read-Encryption-Key") - 1,
					sess->AccessoryToControllerKey, sizeof(sess->AccessoryToControllerKey));

				Hap::Crypt::hkdf(
					(const uint8_t*)"Control-Salt", sizeof("Control-Salt") - 1,
					sess->curve.getSharedSecret(), sess->curve.KeySize,
					(const uint8_t*)"Control-Write-Encryption-Key", sizeof("Control-Write-Encryption-Key") - 1,
					sess->ControllerToAccessoryKey, sizeof(sess->ControllerToAccessoryKey));

				// mark session as secured after response is sent
				sess->ios = ios;

				goto Ret;
			}

		RetErr:	// error
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:
			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}

		void Server::_pairingAdd(Session* sess)
		{
			Tlv::Item id;
			Tlv::Item key;
			Controller::Perm perm;
			const Controller* ios;

			Log("PairingAdd\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M2);

			// verify that controller has admin permissions
			if (sess->ios->perm != Controller::Admin)
			{
				Log("PairingAdd: No Admin permissions\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
				goto Ret;
			}

			// extract required items from input TLV 
			if (!sess->tlvi.get(Tlv::Type::Identifier, id))
			{
				Log("PairingAdd: Identifier not found\n");
				goto RetErr;
			}
			Hex("PairingAdd: Identifier", id.val(), id.len());

			if (!sess->tlvi.get(Tlv::Type::PublicKey, key))
			{
				Log("PairingAdd: PublicKey not found\n");
				goto RetErr;
			}
			Hex("PairingAdd: PublicKey", key.val(), key.len());

			if (!sess->tlvi.get(Tlv::Type::Permissions, perm))
			{
				Log("PairingAdd: Permissions not found\n");
				goto RetErr;
			}
			Log("PairingAdd: Permissions 0x%X\n", perm);

			// locate new controller in pairing db
			ios = _pairings.Get(id);
			if (ios != nullptr)
			{
				// compare controller LTPK with stored one
				if (key.len() != Controller::KeyLen || memcmp(key.val(), ios->key, Controller::KeyLen) != 0)
				{
					Log("PairingAdd: mismatch\n");
					goto RetErr;
				}

				_pairings.Update(id, perm);
			}
			else if (!_pairings.Add(id, key, perm))
			{
				Log("PairingAdd: Unable to add\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::MaxPeers);
				goto Ret;
			}

			Hap::config->Update();

			goto Ret;

		RetErr:	// error
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:
			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}

		void Server::_pairingRemove(Session* sess)
		{
			Tlv::Item id;

			Log("PairingRemove\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M2);

			// verify that controller has admin permissions
			if (sess->ios->perm != Controller::Admin)
			{
				Log("PairingRemove: No Admin permissions\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
				goto Ret;
			}

			// extract required items from input TLV 
			if (!sess->tlvi.get(Tlv::Type::Identifier, id))
			{
				Log("PairingRemove: Identifier not found\n");
				goto RetErr;
			}
			Hex("PairingRemove: Identifier", id.val(), id.len());

			if (!_pairings.Remove(id))
			{
				Log("PairingRemove: Remove error\n");
				goto RetErr;
			}

			Hap::config->Update();

			// TODO: close all sessions to removed controller

			goto Ret;

		RetErr:	// error
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:
			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}

		void Server::_pairingList(Session* sess)
		{
			bool first = true;
			bool rc;

			Log("PairingList\n");

			// prepare response without data
			sess->rsp.start(HTTP_200);
			sess->rsp.add(ContentType, ContentTypeTlv8);
			sess->rsp.add(ContentLength, 0);
			sess->rsp.end();

			// create response TLV in the response buffer right after HTTP headers 
			sess->tlvo.create((uint8_t*)sess->rsp.data(), sess->rsp.size());
			sess->tlvo.add(Hap::Tlv::Type::State, Hap::Tlv::State::M2);

			// verify that controller has admin permissions
			if (sess->ios->perm != Controller::Admin)
			{
				Log("PairingList: No Admin permissions\n");
				sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Authentication);
				goto Ret;
			}

			rc = _pairings.forEach([sess, &first](const Controller* ios) -> bool {

				if (!first)
				{
					if (!sess->tlvo.add(Hap::Tlv::Type::Separator))
						return false;
				}

				if (!sess->tlvo.add(Hap::Tlv::Type::Identifier, ios->id, ios->IdLen))	// TODO: store real ID length (or zero-terminate?)
					return false;

				if (!sess->tlvo.add(Hap::Tlv::Type::PublicKey, ios->key, ios->KeyLen))
					return false;

				if (!sess->tlvo.add(Hap::Tlv::Type::Permissions, ios->perm))
					return false;

				first = false;

				return true;
			});

			if(rc)
				goto Ret;

		//RetErr:	// error
			Log("PairingList: TLV overflow\n");
			sess->tlvo.add(Hap::Tlv::Type::Error, Hap::Tlv::Error::Unknown);

		Ret:
			// adjust content length in response
			sess->rsp.setContentLength(sess->tlvo.length());
		}
	}
}
