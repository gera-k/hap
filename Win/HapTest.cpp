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

#include <SDKDDKVer.h>

#include <tchar.h>
#include <stdio.h> 
#include <string>
#include <iostream>

#include "Hap.h"

extern "C" void t_stronginitrand();

#define Log Hap::Log

#define ACCESSORY_NAME "WinTest"

// convert bin to hex, sizeof(s) must be >= size*2 + 1
void bin2hex(uint8_t* buf, size_t size, char* s)
{
	static const char h[] = "0123456789ABCDEF";

	while (size-- > 0)
	{
		uint8_t b = *buf++;

		*s++ = h[b >> 4];
		*s++ = h[b & 0xF];
	}
	*s++ = 0;
}

void hex2bin(const char* s, uint8_t* buf, size_t size)
{
	uint8_t b;

	while (size-- > 0)
	{
		char c = *s++;
		if (c >= '0' && c <= '9')
			b = c - '0';
		else
			b = c - 'A' + 10;
		b <<= 4;

		c = *s++;
		if (c >= '0' && c <= '9')
			b += c - '0';
		else
			b += c - 'A' + 10;

		*buf++ = b;
	}
}

// HAP database

class MyAccessoryInformation : public Hap::AccessoryInformation
{
public:
	MyAccessoryInformation()
	{
		_identify.onWrite([this](Hap::Obj::wr_prm& p, Hap::Characteristic::Identify::V v) -> void {
			Log("MyAccessoryInformation: write Identify\n");
		});
	}

	void config()
	{
		_manufacturer.Value(Hap::config->manufacturer);
		_model.Value(Hap::config->model);
		_name.Value(Hap::config->name);
		_serialNumber.Value(Hap::config->serialNumber);
		_firmwareRevision.Value(Hap::config->firmwareRevision);
	}

} myAis;

class MyLb : public Hap::Lightbulb
{
private:
	Hap::Characteristic::Brightness _brightness;
	Hap::Characteristic::Name _name;
	int _n;
public:
	MyLb(Hap::Characteristic::Name::V name, int n) : _n(n)
	{
		AddBrightness(_brightness);
		AddName(_name);
		_name.Value(name);

		_on.onRead([this](Hap::Obj::rd_prm& p) -> void {
			Log("MyLb%d: read On: %d\n", _n, _on.Value());
		});

		_on.onWrite([this](Hap::Obj::wr_prm& p, Hap::Characteristic::On::V v) -> void {
			Log("MyLb%d: write On: %d -> %d\n", _n, _on.Value(), v);
		});

		_brightness.onRead([this](Hap::Obj::rd_prm& p) -> void {
			Log("MyLb%d: read Brightness: %d\n", _n, _brightness.Value());
		});

		_brightness.onWrite([this](Hap::Obj::wr_prm& p, Hap::Characteristic::Brightness::V v) -> void {
			Log("MyLb%d: write Brightness: %d -> %d\n", _n, _brightness.Value(), v);
		});
	}

} myLb1("Light-1", 1), myLb2("Light-2", 2);

class MyAcc : public Hap::Accessory<3>
{
public:
	MyAcc() : Hap::Accessory<3>()
	{
		AddService(&myAis);
		AddService(&myLb1);
		AddService(&myLb2);
	}

} myAcc;

class MyDb : public Hap::DbStatic<1>
{
public:
	MyDb()
	{
		AddAcc(&myAcc);
	}

	// db initialization:
	void Init(Hap::iid_t aid)
	{
		// assign instance IDs
		myAcc.setId(aid);

		// config AIS
		myAis.config();
	}

} db;

// Pairing records

class MyPairings : public Hap::Pairings
{
public:
	void Reset()
	{
		init();
	}

	bool Save(FILE* f)
	{
		if (f == NULL)
			return false;

		char* key = new char[Hap::Controller::KeyLen * 2 + 1];

		bool comma = false;
		for (int i = 0; i < sizeofarr(_db); i++)
		{
			Hap::Controller* ios = &_db[i];

			if (ios->perm == Hap::Controller::None)
				continue;

			bin2hex(ios->key, ios->KeyLen, key);

			fprintf(f, "\t\t%c[\"%.*s\",\"%s\",\"%d\"]\n", comma ? ',' :' ', ios->idLen, ios->id, key, ios->perm);
			comma = true;
		}

		delete[] key;

		return true;
	}

	bool Add(const char* id, int id_len, const char* key, int key_len, uint8_t perm)
	{
		if (key_len != Hap::Controller::KeyLen * 2)
			return false;

		uint8_t* key_bin = new uint8_t[Hap::Controller::KeyLen];
		hex2bin(key, key_bin, Hap::Controller::KeyLen);

		bool ret = Pairings::Add((const uint8_t*)id, id_len, key_bin, Hap::Controller::Perm(perm));

		delete[] key_bin;

		return ret;
	}
};

// Crypto keys

class MyCrypto : public Hap::Crypt::Ed25519
{
public:
	void Reset()
	{
		init();
	}

	bool Restore(const char* pub, int pub_len, const char* prv, int prv_len)
	{
		if (pub_len != PubKeySize * 2)
			return false;
		if (prv_len != PrvKeySize * 2)
			return false;

		hex2bin(pub, _pubKey, PubKeySize);
		hex2bin(prv, _prvKey, PrvKeySize);

		return true;
	}

	bool Save(FILE* f)
	{
		if (f == NULL)
			return false;

		char* s = new char[PrvKeySize * 2 + 1];

		bin2hex(_pubKey, PubKeySize, s);
		fprintf(f, "\t\t \"%s\"\n", s);

		bin2hex(_prvKey, PrvKeySize, s);
		fprintf(f, "\t\t,\"%s\"\n", s);

		delete[] s;
		
		return true;
	}
};

// configuration data of this accessory server
//	implements save/restore to/from persistent storage 
class MyConfig : public Hap::Config
{
public:
	MyPairings pairings;
	MyCrypto keys;

	MyConfig(const char* fileName)
		: _fileName(fileName)
	{
		name = _name;
		model = _model;
		manufacturer = _manufacturer;
		serialNumber = _serialNumber;
		firmwareRevision = _firmwareRevision;
		deviceId = _deviceId;
		setupCode = _setupCode;
	}

private:
	const char* _fileName;

	// define storage for string parameters which we save/restore
	char _name[Hap::DefString];				// Accessory name - used as initial Bonjour name and as AIS name of aid=1
	char _model[Hap::DefString];				// Model name (Bonjour and AIS)
	char _manufacturer[Hap::DefString];		// Manufacturer- used by AIS (Accessory Information Service)
	char _serialNumber[Hap::DefString];		// Serial number in arbitrary format
	char _firmwareRevision[Hap::DefString];	// Major[.Minor[.Revision]]
	char _deviceId[Hap::DefString];			// Device ID (XX:XX:XX:XX:XX:XX, new deviceId generated on each factory reset)
	char _setupCode[Hap::DefString];			// setupCode code XXX-XX-XXX

	virtual void _default() override
	{
		Log("Config: reset\n");

		strcpy(_name, ACCESSORY_NAME);
		strcpy(_model, "TestModel");
		strcpy(_manufacturer, "TestMaker");
		strcpy(_serialNumber, "0001");
		strcpy(_firmwareRevision, "0.1");

		// generate new random ID
		uint8_t id[6];
		t_random(id, sizeof(id));
		sprintf(_deviceId, "%02X:%02X:%02X:%02X:%02X:%02X",
			id[0], id[1], id[2], id[3], id[4], id[5]);
		
		configNum = 1;
		categoryId = 5;
		statusFlags = 0
			| Hap::Bonjour::NotPaired
			| Hap::Bonjour::NotConfiguredForWiFi;
		
		strcpy(_setupCode, "000-11-000");
		
		port = swap_16(7889);			// uint16_t port;		// TCP port of HAP service
		BCT = 0;

		pairings.Reset();
		keys.Reset();
	}

	virtual void _reset() override
	{
		Log("Config: reset\n");

		// generate new random ID
		uint8_t id[6];
		t_random(id, sizeof(id));
		sprintf(_deviceId, "%02X:%02X:%02X:%02X:%02X:%02X",
			id[0], id[1], id[2], id[3], id[4], id[5]);

		pairings.Reset();
		keys.Reset();
	}

	virtual bool _save() override
	{
		FILE* f = fopen(_fileName, "w+b");
		if (f == NULL)
		{
			Log("Config: cannot open %s for write\n", _fileName);
			return false;
		}

		Log("Config: save to %s\n", _fileName);

		fprintf(f, "{\n");
		fprintf(f, "\t\"%s\":\"%s\",\n", key[key_name], name);
		fprintf(f, "\t\"%s\":\"%s\",\n", key[key_model], model);
		fprintf(f, "\t\"%s\":\"%s\",\n", key[key_manuf], manufacturer);
		fprintf(f, "\t\"%s\":\"%s\",\n", key[key_serial], serialNumber);
		fprintf(f, "\t\"%s\":\"%s\",\n", key[key_firmware], firmwareRevision);
		fprintf(f, "\t\"%s\":\"%s\",\n", key[key_device], deviceId);
		fprintf(f, "\t\"%s\":\"%d\",\n", key[key_config], configNum);
		fprintf(f, "\t\"%s\":\"%d\",\n", key[key_category], categoryId);
		fprintf(f, "\t\"%s\":\"%d\",\n", key[key_status], statusFlags);
		fprintf(f, "\t\"%s\":\"%s\",\n", key[key_setup], setupCode);
		fprintf(f, "\t\"%s\":\"%d\",\n", key[key_port], swap_16(port));
		fprintf(f, "\t\"%s\":[\n", key[key_keys]);
		keys.Save(f);
		fprintf(f, "\t],\n");
		fprintf(f, "\t\"%s\":[\n", key[key_pairings]);
		pairings.Save(f);
		fprintf(f, "\t]\n");
		fprintf(f, "}\n");

		fclose(f);
		return true;
	}

	virtual bool _restore() override
	{
		Log("Config: restore from %s\n", _fileName);
		FILE* f = fopen(_fileName, "r+b");
		if (f == NULL)
		{
			Log("Config: cannot open %s for read\n", _fileName);
			return false;
		}

		Hap::Json::member om[] =
		{
			{ key[key_name], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_model], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_manuf], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_serial], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_firmware], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_device], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_config], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_category], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_status], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_setup], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_port], Hap::Json::JSMN_STRING | Hap::Json::JSMN_UNDEFINED },
			{ key[key_keys], Hap::Json::JSMN_ARRAY | Hap::Json::JSMN_UNDEFINED },
			{ key[key_pairings], Hap::Json::JSMN_ARRAY | Hap::Json::JSMN_UNDEFINED },
		};
		bool ret = false;
		char* b = nullptr;
		long size = 0;
		Hap::Json::Parser<100> js;
		int rc;

		if (fseek(f, 0, SEEK_END) == 0)
			size = ftell(f);

		if (size <= 0)
			goto Ret;

		b = new char[size];
		if (fseek(f, 0, SEEK_SET) < 0)
			goto Ret;

		if (fread(b, 1, size, f) != size)
			goto Ret;

		if (!js.parse(b, (uint16_t)size))
			goto Ret;
			
		if (js.tk(0)->type != Hap::Json::JSMN_OBJECT)
			goto Ret;

		rc = js.parse(0, om, sizeofarr(om));
		if (rc >= 0)
		{
			Log("parameter '%s' is missing or invalid", om[rc].key);
			goto Ret;
		}

		for (int k = 0; k < key_max; k++)
		{
			int i = om[k].i;
			if (i <= 0)
				continue;

			char s[Hap::DefString + 1];
			js.copy(om[k].i, s, sizeof(s));

			switch (k)
			{
			case key_name:
				js.copy(i, _name, sizeof(_name));
				Log("Config: restore name '%s'\n", name);
				break;
			case key_model:
				js.copy(i, _model, sizeof(_model));
				Log("Config: restore model '%s'\n", model);
				break;
			case key_manuf:
				js.copy(i, _manufacturer, sizeof(_manufacturer));
				Log("Config: restore manufacturer '%s'\n", manufacturer);
				break;
			case key_serial:
				js.copy(i, _serialNumber, sizeof(_serialNumber));
				Log("Config: restore serialNumber '%s'\n", serialNumber);
				break;
			case key_firmware:
				js.copy(i, _firmwareRevision, sizeof(_firmwareRevision));
				Log("Config: restore firmwareRevision '%s'\n", firmwareRevision);
				break;
			case key_device:
				js.copy(i, _deviceId, sizeof(_deviceId));
				Log("Config: restore deviceId '%s'\n", deviceId);
				break;
			case key_config:
				js.is_number<uint32_t>(i, configNum);
				Log("Config: restore configNum '%d'\n", configNum);
				break;
			case key_category:
				js.is_number<uint8_t>(i, categoryId);
				Log("Config: restore categoryId '%d'\n", categoryId);
				break;
			case key_status:
				js.is_number<uint8_t>(i, statusFlags);
				Log("Config: restore statusFlags '%d'\n", statusFlags);
				break;
			case key_setup:
				js.copy(i, _setupCode, sizeof(_setupCode));
				Log("Config: restore setupCode '%s'\n", setupCode);
				break;
			case key_port:
				js.is_number<uint16_t>(i, port);
				Log("Config: restore port '%d'\n", port);
				port = swap_16(port);
				break;
			case key_keys:
				// keys aray must contain exactly two members
				if (js.size(i) == 2)
				{
					int k1 = js.find(i, 0);
					int k2 = js.find(i, 1);
					Log("Config: restore keys '%.*s' '%.*s'\n", js.length(k1), js.start(k1),
						js.length(k2), js.start(k2));
					keys.Restore(js.start(k1), js.length(k1), js.start(k2), js.length(k2));
				}
				else
					keys.Reset();
				break;
			case key_pairings:
				pairings.Reset();
				for (int k = 0; k < js.size(i); k++)
				{
					int r = js.find(i, k);	// pairing record ["id","key","perm"]
					if (js.type(r) == Hap::Json::JSMN_ARRAY && js.size(r) == 3)
					{
						int id = js.find(r, 0);
						int key = js.find(r, 1);
						uint8_t perm;
						if (id > 0 && key > 0 && js.is_number<uint8_t>(js.find(r, 2), perm))
						{
							if (pairings.Add(js.start(id), js.length(id), js.start(key), js.length(key), perm))
								Log("Config: restore pairing '%.*s' '%.*s' %d\n", js.length(id), js.start(id),
									js.length(key), js.start(key), perm);
						}
					}
				}
				break;
			default:
				break;
			}
		}

		ret = true;

	Ret:
		if (b != nullptr)
			delete[] b;
		fclose(f);

		if (!ret)
			Log("Config: cannot read/parse %s\n", _fileName);
		return ret;
	}

} myConfig(ACCESSORY_NAME".hap");

Hap::Config* Hap::config = &myConfig;

// statically allocated storage for HTTP processing
//	Our implementation is single-threaded so only one set of buffers.
//	The http server uses this buffers only during processing a request.
//	All session-persistend data is kept in Session objects.
Hap::BufStatic<char, Hap::MaxHttpFrame * 2> http_req;
Hap::BufStatic<char, Hap::MaxHttpFrame * 4> http_rsp;
Hap::BufStatic<char, Hap::MaxHttpFrame * 1> http_tmp;
Hap::Http::Server::Buf buf = { http_req, http_rsp, http_tmp };
Hap::Http::Server http(buf, db, myConfig.pairings, myConfig.keys);

template<typename T> bool is_number(int i, T& value)
{
	Dbg("unknown number\n");

	return false;
}
template<typename T> bool is_number(std::enable_if_t<std::is_integral<T>::value && std::numeric_limits<T>::is_signed, T&> value)
{
	return true;
}

bool tst()
{
	using type = int32_t;

	type i;

	return is_number<type>(i);

	return false;
}

bool Hap::debug = true;

int main()
{
	tst();
	t_stronginitrand();

	// create servers
	Hap::Mdns* mdns = Hap::Mdns::Create();
	Hap::Tcp* tcp = Hap::Tcp::Create(&http);

	// restore configuration
	myConfig.Init();

	// set config update callback
	myConfig.Update = [mdns]() -> void {

		bool mdnsUpdate = false;

		// see if status flag must change
		bool paired = myConfig.pairings.Count() != 0;
		if (paired && (Hap::config->statusFlags & Hap::Bonjour::NotPaired))
		{
			Hap::config->statusFlags &= ~Hap::Bonjour::NotPaired;
			mdnsUpdate = true;
		}
		else if (!paired && !(Hap::config->statusFlags & Hap::Bonjour::NotPaired))
		{
			Hap::config->statusFlags |= Hap::Bonjour::NotPaired;
			mdnsUpdate = true;
		}
		
		myConfig.Save();

		if (mdnsUpdate)
			mdns->Update();
	};

	// init static objects
	db.Init(1);

#if 1
	// start servers
	mdns->Start();
	tcp->Start();

	// wait for interrupt
	char c;
	std::cin >> c;

	// stop servers
	tcp->Stop();
	mdns->Stop();

#else
	Hap::sid_t sid = http.Open();

	static Hap::BufStatic<char, 4096> buf;
	char* s = buf.ptr();
	int l, len = buf.len();

	l = db.getDb(sid, s, len);
	s[l] = 0;
	printf("sizeof(srv)=%d  db '%s'\n", sizeof(db), s);

	//static const char wr[] = "{\"characteristics\":[{\"aid\":1,\"iid\":2,\"value\":true,\"ev\":true},{\"aid\":3,\"iid\":8,\"ev\":true}]}";
	const char wr[] = "{\"characteristics\":[{\"aid\":1,\"iid\":9,\"value\":true,\"ev\":true},{\"aid\":1,\"iid\":11,\"value\":true,\"ev\":true}]}";

	l = len;
	auto rc = db.Write(sid, wr, sizeof(wr) - 1, s, l);
	Log("Write: %s  rsp '%.*s'\n", Hap::Http::StatusStr(rc), l, s);

	l = len;
	memset(s, 0, l);
	rc = db.getEvents(sid, s, l);
	Log("Events: %s  rsp %d '%.*s'\n", Hap::Http::StatusStr(rc), l, l, s);

	l = len;
	memset(s, 0, l);
	rc = db.getEvents(sid, s, l);
	Log("Events: %s  rsp %d '%.*s'\n", Hap::Http::StatusStr(rc), l, l, s);

	http.Close(sid);

	//sha2_test();
	//srp_test();
	//return 0;

#endif

	return 0;
}

