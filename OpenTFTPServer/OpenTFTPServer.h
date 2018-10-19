/*
	Copyright (C) 2005 by Achal Dhir <achaldhir@gmail.com>
	Copyright (C) 2018  Eddy Beaupré <eddy@beaupre.biz>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
#ifdef _MSC_VER
   #define _CRT_SECURE_NO_WARNINGS
   #define _WINSOCK_DEPRECATED_NO_WARNINGS
   #pragma comment(lib, "ws2_32.lib")
   #pragma comment(lib, "iphlpapi.lib")
#endif
*/

//Constants
constexpr auto MAX_SERVERS = 8;

constexpr auto SERVICE_NAME = "OpenTFTPServer";
constexpr auto SERVICE_DISPLAY_NAME = "Open TFTP Server, MultiThreaded";
constexpr auto SERVICE_VERSION = "2.0.0";
constexpr auto SERVICE_BUILD = __TIMESTAMP__;

constexpr auto FILE_BUFFER_SIZE = 512;

//Structs
struct home
{
	char alias[64];
	char target[256];
};

struct tftpConnType
{
	SOCKET sock;
	sockaddr_in addr;
	unsigned int server;
	unsigned short port;
	bool loaded;
	bool ready;
};

struct acknowledgement
{
	unsigned short opcode;
	unsigned short block;
};

struct message
{
	unsigned short opcode;
	char buffer[514];
};

struct tftperror
{
	unsigned short opcode;
	unsigned short errorcode;
	char errormessage[512];
};

struct packet
{
	unsigned short opcode;
	unsigned short block;
	char buffer;
};

struct data12
{
	unsigned int rangeStart;
	unsigned int rangeEnd;
};

struct request
{
	timeval tv;
	fd_set readfds;
	time_t expiry;
	std::chrono::high_resolution_clock::time_point transfertStart;
	SOCKET sock;
	SOCKET knock;
	unsigned char sockInd;
	unsigned char attempt;
	char path[_MAX_PATH];
	FILE *file;
	char *filename;
	char *mode;
	char *alias;
	unsigned int tsize;
	unsigned int fblock;
	size_t bytesReady;
	size_t bytesRecd;
	size_t bytesRead[2];
	packet* pkt[2];
	sockaddr_in client;
	socklen_t clientsize;
	union
	{
		tftperror serverError;
		message mesout;
		acknowledgement acout;
	};
	union
	{
		tftperror clientError;
		message mesin;
		acknowledgement acin;
	};
	unsigned short blksize;
	unsigned short timeout;
	unsigned short block;
	unsigned short tblock;
	size_t xferSize;
};

struct data1
{
	tftpConnType tftpConn[MAX_SERVERS];
	unsigned int allServers[MAX_SERVERS];
	unsigned int staticServers[MAX_SERVERS];
	unsigned int listenServers[MAX_SERVERS];
	unsigned short listenPorts[MAX_SERVERS];
	SOCKET maxFD;
	bool ready;
	bool busy;
};

struct data2
{
	WSADATA wsaData;
	home homes[8];
	FILE *logfile;
	data12 hostRanges[32];
	char fileRead;
	char fileWrite;
	char fileOverwrite;
	int minport;
	int maxport;
	unsigned int failureCount;
	unsigned char logLevel;
	bool ifspecified;
};

struct data15
{
	union
	{
		//unsigned int ip;
		unsigned ip : 32;
		unsigned char octate[4];
	};
};

//Functions
bool detectChange();
void closeConn();
void getInterfaces(data1*);
void runProg();
void processRequest(LPVOID);
char* myGetToken(char*, unsigned char);
char* myTrim(char*, char*);
void init(void*);
bool cleanReq(request*);
bool addServer(unsigned int*, unsigned int);
FILE* openSection(const char*, unsigned char, char*);
char *readSection(char*, FILE*);
bool getSection(const char*, char*, unsigned char, char*);
bool isIP(char*s);
char* myLower(char*);
char* myUpper(char*);

unsigned int* findServer(unsigned int*, unsigned int);
void printWindowsError();

PSTR vsnprintf_malloc(const char *, va_list);
PSTR ipv4_ntop(const void *);

void logMess(unsigned char, const char *fmt, ...);
void logMess(unsigned char logLevel, request *req, const char *fmt, ...);

