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

#include "pch.h"
#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <process.h>
#include <time.h>
#include <tchar.h>
#include <ws2tcpip.h>
#include <limits.h>
#include <iphlpapi.h>
#include <math.h>
#include <VersionHelpers.h>
#include <signal.h>
#include <string.h>
#include <chrono>

#include "OpenTFTPServer.h"

//Global Variables

char iniFile[_MAX_PATH];
char logFile[_MAX_PATH];
char lnkFile[_MAX_PATH];

unsigned short blksize = 65464;
bool verbatim = false;
unsigned short timeout = 3;
unsigned short loggingDay;
data1 network;
data1 newNetwork;
data2 cfig;
//ThreadPool Variables
HANDLE tEvent;
HANDLE cEvent;
HANDLE sEvent;
HANDLE lEvent;
unsigned char currentServer = UCHAR_MAX;
unsigned short totalThreads = 0;
unsigned short minThreads = 1;
unsigned short activeThreads = 0;

bool isConsoleRunning = true;

//Service Variables
SERVICE_STATUS serviceStatus;
SERVICE_STATUS_HANDLE serviceStatusHandle = 0;
HANDLE stopServiceEvent = 0;

void WINAPI ServiceControlHandler(DWORD controlCode) {
	switch (controlCode) {
	case SERVICE_CONTROL_INTERROGATE:
		break;
	case SERVICE_CONTROL_SHUTDOWN:
	case SERVICE_CONTROL_STOP:
		serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		SetServiceStatus(serviceStatusHandle, &serviceStatus);
		SetEvent(stopServiceEvent);
		return;
	case SERVICE_CONTROL_PAUSE:
		break;
	case SERVICE_CONTROL_CONTINUE:
		break;
	default:
		if (controlCode >= 128 && controlCode <= 255) {
			break;
		} else {
			break;
		}
	}
	SetServiceStatus(serviceStatusHandle, &serviceStatus);
}

void WINAPI ServiceMain(DWORD /*argc*/, TCHAR* /*argv*/[]) {
	serviceStatus.dwServiceType = SERVICE_WIN32;
	serviceStatus.dwCurrentState = SERVICE_STOPPED;
	serviceStatus.dwControlsAccepted = 0;
	serviceStatus.dwWin32ExitCode = NO_ERROR;
	serviceStatus.dwServiceSpecificExitCode = NO_ERROR;
	serviceStatus.dwCheckPoint = 0;
	serviceStatus.dwWaitHint = 0;

	serviceStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceControlHandler);

	if (serviceStatusHandle) {
		serviceStatus.dwCurrentState = SERVICE_START_PENDING;
		SetServiceStatus(serviceStatusHandle, &serviceStatus);

		verbatim = false;

		if (_beginthread(init, 0, 0) == 0) {
			if (cfig.logLevel) {
				logMess(1, "Thread Creation Failed");
			}
			exit(-1);
		}

		fd_set readfds;
		timeval tv;
		int fdsReady = 0;
		tv.tv_sec = 20;
		tv.tv_usec = 0;

		stopServiceEvent = CreateEvent(0, FALSE, FALSE, 0);

		serviceStatus.dwControlsAccepted |= (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
		serviceStatus.dwCurrentState = SERVICE_RUNNING;
		SetServiceStatus(serviceStatusHandle, &serviceStatus);

		do {
			network.busy = false;

			if (!network.tftpConn[0].ready || !network.ready) {
				Sleep(1000);
				continue;
			}

			FD_ZERO(&readfds);

			for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].ready; i++) {
				FD_SET(network.tftpConn[i].sock, &readfds);
			}

			int fdsReady = select((int)network.maxFD, &readfds, NULL, NULL, &tv);

			for (int i = 0; fdsReady > 0 && i < MAX_SERVERS && network.tftpConn[i].ready; i++) {
				if (network.ready) {
					network.busy = true;

					if (FD_ISSET(network.tftpConn[i].sock, &readfds)) {
						WaitForSingleObject(sEvent, INFINITE);
						currentServer = i;

						if (!totalThreads || activeThreads >= totalThreads) {
							_beginthread(processRequest, 0, NULL);
						}

						SetEvent(tEvent);
						WaitForSingleObject(sEvent, INFINITE);
						fdsReady--;
						SetEvent(sEvent);
					}
				}
			}
		} while (WaitForSingleObject(stopServiceEvent, 0) == WAIT_TIMEOUT);

		serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
		SetServiceStatus(serviceStatusHandle, &serviceStatus);

		logMess(1, "Closing Network Connections...");

		closeConn();

		WSACleanup();

		logMess(1, "%s (Service mode) stopped", SERVICE_DISPLAY_NAME);

		if (cfig.logfile) {
			fclose(cfig.logfile);
			cfig.logfile = NULL;
		}

		serviceStatus.dwControlsAccepted &= ~(SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
		serviceStatus.dwCurrentState = SERVICE_STOPPED;
		SetServiceStatus(serviceStatusHandle, &serviceStatus);
		CloseHandle(stopServiceEvent);
		stopServiceEvent = 0;
	}
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
	if (isConsoleRunning) {
		switch (fdwCtrlType) {
			// Handle the CTRL-C signal. 
		case CTRL_C_EVENT:
			logMess(1, "Caught CTRL-C Event. Shutting down");
			isConsoleRunning = false;
			return TRUE;
			// CTRL-CLOSE: confirm that the user wants to exit. 
		case CTRL_CLOSE_EVENT:
			logMess(1, "Caught CLOSE Event. Shutting down");
			isConsoleRunning = false;
			return TRUE;
			// Pass other signals to the next handler. 
		case CTRL_BREAK_EVENT:
			logMess(1, "Caught CTRL-BREAK Event. Shutting down");
			isConsoleRunning = false;
			return TRUE;
		case CTRL_LOGOFF_EVENT:
			logMess(1, "Caught LOGOFF Event. Shutting down");
			isConsoleRunning = false;
			return FALSE;
		case CTRL_SHUTDOWN_EVENT:
			logMess(1, "Caught SHUTDOWN Event. Shutting down");
			isConsoleRunning = false;
			return FALSE;
		default:
			return FALSE;
		}
	} else {
		logMess(1, "Shutdown in progress, please wait...");
	}
	return FALSE;
}

void runService() {
	SERVICE_TABLE_ENTRY serviceTable[] = { {(LPSTR)SERVICE_NAME, ServiceMain}, {0, 0} };
	StartServiceCtrlDispatcher(serviceTable);
}

bool stopService(SC_HANDLE service) {
	if (service) {
		SERVICE_STATUS serviceStatus;
		QueryServiceStatus(service, &serviceStatus);
		if (serviceStatus.dwCurrentState != SERVICE_STOPPED) {
			ControlService(service, SERVICE_CONTROL_STOP, &serviceStatus);
			logMess(1, "Stopping Service.");
			for (int i = 0; i < 100; i++) {
				QueryServiceStatus(service, &serviceStatus);
				if (serviceStatus.dwCurrentState == SERVICE_STOPPED) {
					logMess(1, "Stopped");
					return true;
				} else {
					Sleep(500);
				}
			}
			logMess(1, "Failed\n");
			return false;
		}
	}
	return true;
}

void installService() {
	SC_HANDLE serviceControlManager = OpenSCManager(0, 0, SC_MANAGER_CREATE_SERVICE);

	if (serviceControlManager) {
		SC_HANDLE service = OpenService(serviceControlManager, SERVICE_NAME, SERVICE_QUERY_STATUS);
		if (service) {
			logMess(1, "Service Already Exists..");
			StartService(service, 0, NULL);
			CloseServiceHandle(service);
		} else {
			TCHAR path[_MAX_PATH + 1];
			if (GetModuleFileName(0, path, sizeof(path) / sizeof(path[0])) > 0) {
				SC_HANDLE service = CreateService(serviceControlManager, SERVICE_NAME, SERVICE_DISPLAY_NAME, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_IGNORE, path, 0, 0, 0, 0, 0);
				if (service) {
					logMess(1, "Successfully installed");
					StartService(service, 0, NULL);
					CloseServiceHandle(service);
				} else {
					logMess(1, "Installation Failed");
				}
			}
		}
		CloseServiceHandle(serviceControlManager);
	} else {
		printWindowsError();
	}

}

void uninstallService() {
	SC_HANDLE serviceControlManager = OpenSCManager(0, 0, SC_MANAGER_CONNECT);

	if (serviceControlManager) {
		SC_HANDLE service = OpenService(serviceControlManager, SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
		if (service) {
			if (stopService(service)) {
				DeleteService(service);
				logMess(1, "Successfully Removed");
			} else {
				logMess(1, "Failed to Stop Service");
			}
			CloseServiceHandle(service);
		}
		CloseServiceHandle(serviceControlManager);
	} else {
		printWindowsError();
	}

}

void printWindowsError() {
	unsigned int dw = GetLastError();

	if (dw) {
		LPVOID lpMsgBuf;

		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&lpMsgBuf, 0, NULL);
		logMess(1, "Error: %s", (char *)lpMsgBuf);
	}
}

int main(int argc, TCHAR* argv[]) {
	if (IsWindowsXPOrGreater()) {
		if (argc > 1 && lstrcmpi(argv[1], TEXT("-i")) == 0) {
			installService();
		} else if (argc > 1 && lstrcmpi(argv[1], TEXT("-u")) == 0) {
			uninstallService();
		} else if (argc > 1 && lstrcmpi(argv[1], TEXT("-v")) == 0) {
			SC_HANDLE serviceControlManager = OpenSCManager(0, 0, SC_MANAGER_CONNECT);
			bool serviceStopped = true;

			if (serviceControlManager) {
				SC_HANDLE service = OpenService(serviceControlManager, SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_STOP);
				if (service) {
					serviceStopped = stopService(service);
					CloseServiceHandle(service);
				}
				CloseServiceHandle(serviceControlManager);
			} else {
				printWindowsError();
			}

			if (serviceStopped) {
				ConsoleMain();
			} else {
				logMess(1, "Failed to Stop Service");
			}
		} else {
			runService();
		}
	} else if (argc == 1 || lstrcmpi(argv[1], TEXT("-v")) == 0) {
		ConsoleMain();
	} else {
		logMess(1, "This option is not available on Windows95/98/ME");
	}

	logMess(1, "Exiting");
	return 0;
}

void ConsoleMain() {
	verbatim = true;

	if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
		logMess(1, "Error: Could not set ctrl-break handler");
	}

	if (_beginthread(init, 0, 0) == 0) {
		if (cfig.logLevel) {
			logMess(1, "Thread Creation Failed");
		}
		exit(-1);
	}

	fd_set readfds;
	timeval tv;
	int fdsReady = 0;
	tv.tv_sec = 20;
	tv.tv_usec = 0;

	do {
		network.busy = false;

		if (!network.tftpConn[0].ready || !network.ready) {
			Sleep(1000);
			continue;
		}

		FD_ZERO(&readfds);

		for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].ready; i++) {
			FD_SET(network.tftpConn[i].sock, &readfds);
		}

		fdsReady = select((int)network.maxFD, &readfds, NULL, NULL, &tv);

		if (!network.ready) {
			continue;
		}

		for (int i = 0; fdsReady > 0 && i < MAX_SERVERS && network.tftpConn[i].ready; i++) {
			if (network.ready) {
				network.busy = true;

				if (FD_ISSET(network.tftpConn[i].sock, &readfds)) {
					WaitForSingleObject(sEvent, INFINITE);
					currentServer = i;

					if (!totalThreads || activeThreads >= totalThreads) {
						_beginthread(processRequest, 0, NULL);
					}

					SetEvent(tEvent);
					WaitForSingleObject(sEvent, INFINITE);
					fdsReady--;
					SetEvent(sEvent);
				}
			}
		}
	} while (isConsoleRunning);
	logMess(1, "Shutdown completed, cleaning up");
	closeConn();
	WSACleanup();
}

void closeConn() {
	for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].loaded; i++)
		if (network.tftpConn[i].ready)
			closesocket(network.tftpConn[i].sock);
}

void processRequest(void *lpParam) {
	request req;

	WaitForSingleObject(cEvent, INFINITE);
	totalThreads++;
	SetEvent(cEvent);

	do {
		WaitForSingleObject(tEvent, INFINITE);

		WaitForSingleObject(cEvent, INFINITE);
		activeThreads++;
		SetEvent(cEvent);

		if (currentServer >= MAX_SERVERS || !network.tftpConn[currentServer].port) {
			SetEvent(sEvent);
			req.attempt = UCHAR_MAX;
			continue;
		}

		memset(&req, 0, sizeof(request));
		req.sock = INVALID_SOCKET;

		req.clientsize = sizeof(req.client);
		req.sockInd = currentServer;
		currentServer = UCHAR_MAX;
		req.knock = network.tftpConn[req.sockInd].sock;

		if (req.knock == INVALID_SOCKET) {
			SetEvent(sEvent);
			req.attempt = UCHAR_MAX;
			continue;
		}

		errno = 0;
		req.bytesRecd = recvfrom(req.knock, (char*)&req.mesin, sizeof(message), 0, (sockaddr*)&req.client, &req.clientsize);
		errno = WSAGetLastError();

		SetEvent(sEvent);

		if (!errno && req.bytesRecd > 0) {
			if (cfig.hostRanges[0].rangeStart) {
				unsigned int iip = ntohl(req.client.sin_addr.s_addr);
				bool allowed = false;

				for (int j = 0; j <= 32 && cfig.hostRanges[j].rangeStart; j++) {
					if (iip >= cfig.hostRanges[j].rangeStart && iip <= cfig.hostRanges[j].rangeEnd) {
						allowed = true;
						break;
					}
				}

				if (!allowed) {
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(2);
					logMess(1, &req, "Access Denied");
					sendto(req.knock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client, req.clientsize);
					req.attempt = UCHAR_MAX;
					continue;
				}
			}

			if ((htons(req.mesin.opcode) == 5)) {
				logMess(2, &req, "Error Code %i at Client, %s", ntohs(req.clientError.errorcode), req.clientError.errormessage);
				req.attempt = UCHAR_MAX;
				continue;
			} else if (htons(req.mesin.opcode) != 1 && htons(req.mesin.opcode) != 2) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(5);
				logMess(2, &req, "Unknown Transfer Id");
				sendto(req.knock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client, req.clientsize);
				req.attempt = UCHAR_MAX;
				continue;
			}
		} else {
			logMess(1, &req, "Communication Error");
			req.attempt = UCHAR_MAX;
			continue;
		}

		req.blksize = 512;
		req.timeout = timeout;
		req.expiry = time(NULL) + req.timeout;
		bool fetchAck = false;

		req.sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

		if (req.sock == INVALID_SOCKET) {
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(0);
			logMess(1, &req, "Thread Socket Creation Error");
			sendto(req.knock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client, req.clientsize);
			req.attempt = UCHAR_MAX;
			continue;
		}

		sockaddr_in service;
		service.sin_family = AF_INET;
		service.sin_addr.s_addr = network.tftpConn[req.sockInd].server;

		if (cfig.minport) {
			for (unsigned short comport = cfig.minport; ; comport++) {
				service.sin_port = htons(comport);

				if (comport > cfig.maxport) {
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(0);
					logMess(1, &req, "No port is free");
					sendto(req.knock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client, req.clientsize);
					req.attempt = UCHAR_MAX;
					break;
				} else if (bind(req.sock, (sockaddr*)&service, sizeof(service)) == -1) {
					continue;
				} else {
					break;
				}
			}
		} else {
			service.sin_port = 0;

			if (bind(req.sock, (sockaddr*)&service, sizeof(service)) == -1) {
				logMess(1, &req, "Thread failed to bind");
				sendto(req.knock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client, req.clientsize);
				req.attempt = UCHAR_MAX;
			}
		}

		if (req.attempt >= 3) {
			continue;
		}

		if (connect(req.sock, (sockaddr*)&req.client, req.clientsize) == -1) {
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(0);
			logMess(1, &req, "Connect Failed");
			sendto(req.knock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client, req.clientsize);
			req.attempt = UCHAR_MAX;
			continue;
		}

		char *inPtr = req.mesin.buffer;
		*(inPtr + (req.bytesRecd - 3)) = 0;
		req.filename = inPtr;

		if (!strlen(req.filename) || strlen(req.filename) > UCHAR_MAX) {
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(4);
			logMess(1, &req, "Malformed Request, Invalid / Missing Filename");
			send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
			req.attempt = UCHAR_MAX;
			continue;
		}

		inPtr += strlen(inPtr) + 1;
		req.mode = inPtr;

		if (!strlen(req.mode) || strlen(req.mode) > 25) {
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(4);
			logMess(1, &req, "Malformed Request, Invalid/Missing Mode");
			send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
			req.attempt = UCHAR_MAX;
			continue;
		}

		inPtr += strlen(inPtr) + 1;

		for (unsigned int i = 0; i < strlen(req.filename); i++) {
			if (req.filename[i] == '/') {
				req.filename[i] = '\\';
			}
		}

		if (strstr(req.filename, "..\\")) {
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(2);
			logMess(1, &req, "Access violation");
			send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
			req.attempt = UCHAR_MAX;
			continue;
		}

		if (req.filename[0] == '\\') {
			req.filename++;
		}

		if (!cfig.homes[0].alias[0]) {
			if (strlen(cfig.homes[0].target) + strlen(req.filename) >= sizeof(req.path)) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(4);
				logMess(1, &req, "Filename too large");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			strcpy_s(req.path, sizeof(req.path), cfig.homes[0].target);
			strcat_s(req.path, sizeof(req.path), req.filename);
		} else {
			char *bname = strchr(req.filename, '\\');

			if (bname) {
				*bname = 0;
				bname++;
			} else {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "Missing directory/alias");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			for (int i = 0; i < 8; i++) {
				if (cfig.homes[i].alias[0] && !_stricmp(req.filename, cfig.homes[i].alias)) {
					if (strlen(cfig.homes[i].target) + strlen(bname) >= sizeof(req.path)) {
						req.serverError.opcode = htons(5);
						req.serverError.errorcode = htons(4);
						logMess(1, &req, "Filename too large");
						send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
						req.attempt = UCHAR_MAX;
						break;
					}

					strcpy_s(req.path, sizeof(req.path), cfig.homes[i].target);
					strcat_s(req.path, sizeof(req.path), bname);
					break;
				} else if (i == 7 || !cfig.homes[i].alias[0]) {
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(2);
					logMess(1, &req, "No such directory/alias %s", req.filename);
					send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
					req.attempt = UCHAR_MAX;
					break;
				}
			}
		}

		if (req.attempt >= 3)
			continue;

		if (ntohs(req.mesin.opcode) == 1) {
			if (!cfig.fileRead) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "GET Access Denied");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			if (*inPtr) {
				char *tmp = inPtr;

				while (*tmp) {
					if (!_stricmp(tmp, "blksize")) {
						tmp += strlen(tmp) + 1;
						unsigned int val = atol(tmp);

						if (val < 512) {
							val = 512;
						} else if (val > blksize) {
							val = blksize;
						}

						req.blksize = val;
						break;
					}
					tmp += strlen(tmp) + 1;
				}
			}

			errno = 0;

			if (!_stricmp(req.mode, "netascii") || !_stricmp(req.mode, "ascii")) {
				fopen_s(&req.file, req.path, "rt");
			} else {
				fopen_s(&req.file, req.path, "rb");
			}

			req.transfertStart = std::chrono::high_resolution_clock::now();
			logMess(1, &req, "Sending file, transfert mode: %s", req.mode);

			if (errno || !req.file) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(1);
				logMess(1, &req, "File not found or No Access");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}
		} else {
			if (!cfig.fileWrite && !cfig.fileOverwrite) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "PUT Access Denied");
				req.attempt = UCHAR_MAX;
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				continue;
			}

			fopen_s(&req.file, req.path, "rb");

			if (req.file) {
				fclose(req.file);
				req.file = NULL;

				if (!cfig.fileOverwrite) {
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(6);
					logMess(1, &req, "File already exists");
					send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
					req.attempt = UCHAR_MAX;
					continue;
				}
			} else if (!cfig.fileWrite) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "Create File Access Denied");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			errno = 0;

			if (!_stricmp(req.mode, "netascii") || !_stricmp(req.mode, "ascii")) {
				fopen_s(&req.file, req.path, "wt");
			} else {
				fopen_s(&req.file, req.path, "wb");
			}

			req.transfertStart = std::chrono::high_resolution_clock::now();
			logMess(1, &req, "Receiving file, transfert mode: %s", req.mode);

			if (errno || !req.file) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "Invalid Path or No Access");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}
		}

		setvbuf(req.file, NULL, _IOFBF, 5 * req.blksize);

		if (*inPtr) {
			fetchAck = true;
			char *outPtr = req.mesout.buffer;
			req.mesout.opcode = htons(6);
			unsigned int val;
			while (*inPtr) {
				if (!_stricmp(inPtr, "blksize")) {
					strcpy_s(outPtr, sizeof(outPtr), inPtr);
					outPtr += strlen(outPtr) + 1;
					inPtr += strlen(inPtr) + 1;
					val = atol(inPtr);

					if (val < 512) {
						val = 512;
					} else if (val > blksize) {
						val = blksize;
					}

					req.blksize = val;
					sprintf_s(outPtr, sizeof(outPtr), "%u", val);
					outPtr += strlen(outPtr) + 1;
				} else if (!_stricmp(inPtr, "tsize")) {
					strcpy_s(outPtr, sizeof(outPtr), inPtr);
					outPtr += strlen(outPtr) + 1;
					inPtr += strlen(inPtr) + 1;

					if (ntohs(req.mesin.opcode) == 1) {
						if (!fseek(req.file, 0, SEEK_END)) {
							if (ftell(req.file) >= 0) {
								req.tsize = ftell(req.file);
								sprintf_s(outPtr, sizeof(outPtr), "%u", req.tsize);
								outPtr += strlen(outPtr) + 1;
							} else {
								req.serverError.opcode = htons(5);
								req.serverError.errorcode = htons(2);
								logMess(1, &req, "Invalid Path or No Access");
								send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
								req.attempt = UCHAR_MAX;
								break;
							}
						} else {
							req.serverError.opcode = htons(5);
							req.serverError.errorcode = htons(2);
							logMess(1, &req, "Invalid Path or No Access");
							send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
							req.attempt = UCHAR_MAX;
							break;
						}
					} else {
						req.tsize = 0;
						sprintf_s(outPtr, sizeof(outPtr), "%u", req.tsize);
						outPtr += strlen(outPtr) + 1;
					}
				} else if (!_stricmp(inPtr, "timeout")) {
					strcpy_s(outPtr, sizeof(outPtr), inPtr);
					outPtr += strlen(outPtr) + 1;
					inPtr += strlen(inPtr) + 1;
					val = atoi(inPtr);

					if (val < 1) {
						val = 1;
					} else if (val > UCHAR_MAX) {
						val = UCHAR_MAX;
					}

					req.timeout = val;
					req.expiry = time(NULL) + req.timeout;
					sprintf_s(outPtr, sizeof(outPtr), "%u", val);
					outPtr += strlen(outPtr) + 1;
				}

				inPtr += strlen(inPtr) + 1;
			}

			if (req.attempt >= 3) {
				continue;
			}

			errno = 0;
			req.bytesReady = (size_t)outPtr - (size_t)&req.mesout;
			send(req.sock, (const char*)&req.mesout, (int)req.bytesReady, 0);
			errno = WSAGetLastError();
		} else if (htons(req.mesin.opcode) == 2) {
			req.acout.opcode = htons(4);
			req.acout.block = htons(0);
			errno = 0;
			req.bytesReady = 4;
			send(req.sock, (const char*)&req.mesout, (int)req.bytesReady, 0);
			errno = WSAGetLastError();
		}

		if (errno) {
			logMess(1, &req, "Communication Error");
			req.attempt = UCHAR_MAX;
			continue;
		} else if (ntohs(req.mesin.opcode) == 1) {
			errno = 0;
			req.pkt[0] = (packet*)calloc(1, req.blksize + 4);
			req.pkt[1] = (packet*)calloc(1, req.blksize + 4);

			if (errno || !req.pkt[0] || !req.pkt[1]) {
				logMess(1, &req, "Memory Error");
				req.attempt = UCHAR_MAX;
				continue;
			}

			long ftellLoc = ftell(req.file);

			if (ftellLoc > 0) {
				if (fseek(req.file, 0, SEEK_SET)) {
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(2);
					logMess(1, &req, "File Access Error");
					send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
					req.attempt = UCHAR_MAX;
					continue;
				}
			} else if (ftellLoc < 0) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "File Access Error");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			errno = 0;
			req.pkt[0]->opcode = htons(3);
			req.pkt[0]->block = htons(1);
			req.bytesRead[0] = fread(&req.pkt[0]->buffer, 1, req.blksize, req.file);
			req.xferSize += req.bytesRead[0];

			if (errno) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "Invalid Path or No Access");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			if (req.bytesRead[0] == req.blksize) {
				req.pkt[1]->opcode = htons(3);
				req.pkt[1]->block = htons(2);
				req.bytesRead[1] = fread(&req.pkt[1]->buffer, 1, req.blksize, req.file);
				req.xferSize += req.bytesRead[1];
				if (req.bytesRead[1] < req.blksize) {
					fclose(req.file);
					req.file = 0;
				}
			} else {
				fclose(req.file);
				req.file = 0;
			}

			if (errno) {
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				logMess(1, &req, "Invalid Path or No Access");
				send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			while (req.attempt <= 3) {
				if (fetchAck) {
					FD_ZERO(&req.readfds);
					req.tv.tv_sec = 1;
					req.tv.tv_usec = 0;
					FD_SET(req.sock, &req.readfds);
					select((int)req.sock + 1, &req.readfds, NULL, NULL, &req.tv);

					if (FD_ISSET(req.sock, &req.readfds)) {
						errno = 0;
						req.bytesRecd = recv(req.sock, (char*)&req.mesin, sizeof(message), 0);
						errno = WSAGetLastError();
						if (req.bytesRecd <= 0 || errno) {
							logMess(1, &req, "Communication Error");
							req.attempt = UCHAR_MAX;
							break;
						} else if (req.bytesRecd >= 4 && ntohs(req.mesin.opcode) == 4) {
							if (ntohs(req.acin.block) == req.block) {
								req.block++;
								req.fblock++;
								req.attempt = 0;
							} else if (req.expiry > time(NULL)) {
								continue;
							} else {
								req.attempt++;
							}
						} else if (ntohs(req.mesin.opcode) == 5) {
							PSTR tmpIp = ipv4_ntop(&req.client.sin_addr);
							logMess(1, &req, "Client %s:%u, Error Code %i at Client, %s", tmpIp, ntohs(req.client.sin_port), ntohs(req.clientError.errorcode), req.clientError.errormessage);
							free(tmpIp);
							req.attempt = UCHAR_MAX;
							break;
						} else {
							req.serverError.opcode = htons(5);
							req.serverError.errorcode = htons(4);
							logMess(1, &req, "Unexpected Option Code %i", ntohs(req.mesin.opcode));
							send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
							req.attempt = UCHAR_MAX;
							break;
						}
					} else if (req.expiry > time(NULL)) {
						continue;
					} else {
						req.attempt++;
					}
				} else {
					fetchAck = true;
					req.acin.block = 1;
					req.block = 1;
					req.fblock = 1;
				}

				if (req.attempt >= 3) {
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(0);

					if (req.fblock && !req.block) {
						logMess(1, &req, "Large File, Block# Rollover not supported by Client");
					} else {
						logMess(1, &req, "Timeout");
					}
					send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
					req.attempt = UCHAR_MAX;
					break;
				} else if (!req.fblock) {
					errno = 0;
					send(req.sock, (const char*)&req.mesout, (int)req.bytesReady, 0);
					errno = WSAGetLastError();
					if (errno) {
						logMess(1, &req, "Communication Error");
						req.attempt = UCHAR_MAX;
						break;
					}
					req.expiry = time(NULL) + req.timeout;
				} else if (ntohs(req.pkt[0]->block) == req.block) {
					errno = 0;
					send(req.sock, (const char*)req.pkt[0], (int)req.bytesRead[0] + 4, 0);
					errno = WSAGetLastError();
					if (errno) {
						logMess(1, &req, "Communication Error");
						req.attempt = UCHAR_MAX;
						break;
					}
					req.expiry = time(NULL) + req.timeout;

					if (req.file) {
						req.tblock = ntohs(req.pkt[1]->block) + 1;
						if (req.tblock == req.block) {
							req.pkt[1]->block = htons(++req.tblock);
							req.bytesRead[1] = fread(&req.pkt[1]->buffer, 1, req.blksize, req.file);
							req.xferSize += req.bytesRead[1];

							if (errno) {
								char errBuff[256];
								strerror_s(errBuff, sizeof(errBuff), errno);
								req.serverError.opcode = htons(5);
								req.serverError.errorcode = htons(4);
								logMess(1, &req, errBuff);
								send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
								req.attempt = UCHAR_MAX;
								break;
							} else if (req.bytesRead[1] < req.blksize) {
								fclose(req.file);
								req.file = 0;
							}
						}
					}
				} else if (ntohs(req.pkt[1]->block) == req.block) {
					errno = 0;
					send(req.sock, (const char*)req.pkt[1], (int)req.bytesRead[1] + 4, 0);
					errno = WSAGetLastError();
					if (errno) {
						char errBuff[256];
						strerror_s(errBuff, sizeof(errBuff), errno);

						logMess(1, &req, "Communication Error: %s", errBuff);
						req.attempt = UCHAR_MAX;
						break;
					}

					req.expiry = time(NULL) + req.timeout;

					if (req.file) {
						req.tblock = ntohs(req.pkt[0]->block) + 1;
						if (req.tblock == req.block) {
							req.pkt[0]->block = htons(++req.tblock);
							req.bytesRead[0] = fread(&req.pkt[0]->buffer, 1, req.blksize, req.file);
							req.xferSize += req.bytesRead[0];
							if (errno) {
								char errBuff[256];
								req.serverError.opcode = htons(5);
								req.serverError.errorcode = htons(4);
								strerror_s(errBuff, sizeof(errBuff), errno);
								logMess(1, &req, errBuff);
								send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
								req.attempt = UCHAR_MAX;
								break;
							} else if (req.bytesRead[0] < req.blksize) {
								fclose(req.file);
								req.file = 0;
							}
						}
					}
				} else {
					std::chrono::duration<double> elapsed = std::chrono::high_resolution_clock::now() - req.transfertStart;
					logMess(2, &req, "%d bytes (%u Blocks) sent in %0.2f seconds, %0.2f bytes/s", req.xferSize, req.fblock - 1, elapsed.count(), req.xferSize / elapsed.count());
					req.attempt = UCHAR_MAX;
					break;
				}
			}
		} else if (ntohs(req.mesin.opcode) == 2) {
			errno = 0;
			req.pkt[0] = (packet*)calloc(1, req.blksize + 4);

			if (errno || !req.pkt[0]) {
				logMess(1, &req, "Memory Error");
				req.attempt = UCHAR_MAX;
				continue;
			}

			while (req.attempt <= 3) {
				FD_ZERO(&req.readfds);
				req.tv.tv_sec = 1;
				req.tv.tv_usec = 0;
				FD_SET(req.sock, &req.readfds);
				select((int)req.sock + 1, &req.readfds, NULL, NULL, &req.tv);

				if (FD_ISSET(req.sock, &req.readfds)) {
					errno = 0;
					req.bytesRecd = recv(req.sock, (char*)req.pkt[0], req.blksize + 4, 0);
					errno = WSAGetLastError();

					if (errno) {
						logMess(1, &req, "Communication Error");
						req.attempt = UCHAR_MAX;
						break;
					}
				} else {
					req.bytesRecd = 0;
				}

				if (req.bytesRecd >= 4) {
					if (ntohs(req.pkt[0]->opcode) == 3) {
						req.tblock = req.block + 1;

						if (ntohs(req.pkt[0]->block) == req.tblock) {
							req.acout.opcode = htons(4);
							req.acout.block = req.pkt[0]->block;
							req.block++;
							req.fblock++;
							req.bytesReady = 4;
							req.expiry = time(NULL) + req.timeout;

							errno = 0;
							send((int)req.sock, (const char*)&req.mesout, (int)req.bytesReady, 0);
							errno = WSAGetLastError();

							if (errno) {
								logMess(1, &req, "Communication Error");
								req.attempt = UCHAR_MAX;
								break;
							}

							if (req.bytesRecd > 4) {
								errno = 0;
								req.xferSize += (req.bytesRecd - 4);
								if (fwrite(&req.pkt[0]->buffer, req.bytesRecd - 4, 1, req.file) != 1 || errno) {
									req.serverError.opcode = htons(5);
									req.serverError.errorcode = htons(3);
									logMess(1, &req, "Disk full or allocation exceeded");
									send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
									req.attempt = UCHAR_MAX;
									break;
								} else {
									req.attempt = 0;
								}
							} else {
								req.attempt = 0;
							}

							if ((unsigned short)req.bytesRecd < req.blksize + 4) {
								time_t timeNow;
								fclose(req.file);
								req.file = 0;
								time(&timeNow);
								std::chrono::duration<double> elapsed = std::chrono::high_resolution_clock::now() - req.transfertStart;
								logMess(2, &req, "%d bytes (%u Blocks) received in %0.2f seconds, %0.2f bytes/s", req.xferSize, req.fblock - 1, elapsed.count(), req.xferSize / elapsed.count());
								req.attempt = UCHAR_MAX;
								break;
							}
						} else if (req.expiry > time(NULL)) {
							continue;
						} else if (req.attempt >= 3) {
							req.serverError.opcode = htons(5);
							req.serverError.errorcode = htons(0);

							if (req.fblock && !req.block) {
								logMess(1, &req, "Large File, Block# Rollover not supported by Client");
							} else {
								logMess(1, &req, "Timeout");
							}


							send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
							req.attempt = UCHAR_MAX;
							break;
						} else {
							req.expiry = time(NULL) + req.timeout;
							errno = 0;
							send(req.sock, (const char*)&req.mesout, (int)req.bytesReady, 0);
							errno = WSAGetLastError();
							req.attempt++;

							if (errno) {
								logMess(1, &req, "Communication Error");
								req.attempt = UCHAR_MAX;
								break;
							}
						}
					} else if (req.bytesRecd > (int)sizeof(message)) {
						req.serverError.opcode = htons(5);
						req.serverError.errorcode = htons(4);
						logMess(1, &req, "Error: Incoming Packet too large");
						send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
						req.attempt = UCHAR_MAX;
						break;
					} else if (ntohs(req.pkt[0]->opcode) == 5) {
						logMess(1, &req, "Error Code %i at Client, %s", ntohs(req.pkt[0]->block), &req.pkt[0]->buffer);
						req.attempt = UCHAR_MAX;
						break;
					} else {
						req.serverError.opcode = htons(5);
						req.serverError.errorcode = htons(4);
						logMess(1, &req, "Unexpected Option Code %i", ntohs(req.pkt[0]->opcode));
						send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
						req.attempt = UCHAR_MAX;
						break;
					}
				} else if (req.expiry > time(NULL)) {
					continue;
				} else if (req.attempt >= 3) {
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(0);

					if (req.fblock && !req.block) {
						logMess(1, &req, "Large File, Block# Rollover not supported by Client");
					} else {
						logMess(1, &req, "Timeout");
					}
					send(req.sock, (const char*)&req.serverError, (int)strlen(req.serverError.errormessage) + 5, 0);
					req.attempt = UCHAR_MAX;
					break;
				} else {
					req.expiry = time(NULL) + req.timeout;
					errno = 0;
					send(req.sock, (const char*)&req.mesout, (int)req.bytesReady, 0);
					errno = WSAGetLastError();
					req.attempt++;

					if (errno) {
						logMess(1, &req, "Communication Error");
						req.attempt = UCHAR_MAX;
						break;
					}
				}
			}
		}
	} while (cleanReq(&req));

	WaitForSingleObject(cEvent, INFINITE);
	totalThreads--;
	SetEvent(cEvent);

	_endthread();
	return;
}

bool cleanReq(request* req) {
	if (req->file) {
		fclose(req->file);
	}

	if (!(req->sock == INVALID_SOCKET)) {
		closesocket(req->sock);
	}

	if (req->pkt[0]) {
		free(req->pkt[0]);
	}

	if (req->pkt[1]) {
		free(req->pkt[1]);
	}

	WaitForSingleObject(cEvent, INFINITE);
	activeThreads--;
	SetEvent(cEvent);

	return (totalThreads <= minThreads);
}

bool getSection(const char *sectionName, char *buffer, unsigned char serial, char *fileName) {
	char section[128];
	FILE *f;
	char buff[FILE_BUFFER_SIZE];
	unsigned char found = 0;

	sprintf_s(section, FILE_BUFFER_SIZE - 1, "[%s]", sectionName);
	myUpper(section);

	fopen_s(&f, fileName, "rt");

	if (f) {
		while (fgets(buff, sizeof(buff) - 1, f)) {
			myUpper(buff);
			myTrim(buff, buff);

			if (strstr(buff, section) == buff) {
				found++;
				if (found == serial) {
					while (fgets(buff, 511, f)) {
						myTrim(buff, buff);

						if (strstr(buff, "[") == buff) {
							break;
						}

						if ((*buff) >= '0' && (*buff) <= '9' || (*buff) >= 'A' && (*buff) <= 'Z' || (*buff) >= 'a' && (*buff) <= 'z' || ((*buff) && strchr("/\\?*", (*buff)))) {
							buffer += sprintf_s(buffer, sizeof(buffer), "%s", buff);
							buffer++;
						}
					}
					break;
				}
			}
		}
		fclose(f);
	}

	*buffer = 0;
	*(buffer + 1) = 0;
	return (found == serial);
}

FILE *openSection(const char *sectionName, unsigned char serial, char *fileName) {
	char section[128];
	FILE *f;
	char buff[FILE_BUFFER_SIZE];
	unsigned char found = 0;

	sprintf_s(section, sizeof(section), "[%s]", sectionName);
	myUpper(section);

	fopen_s(&f, fileName, "rt");

	if (f) {
		while (fgets(buff, sizeof(buff) - 1, f)) {
			myUpper(buff);
			myTrim(buff, buff);

			if (strstr(buff, section) == buff) {
				found++;

				if (found == serial) {
					return f;
				}

			}
		}
		fclose(f);
	}
	return NULL;
}

char *readSection(char* buff, FILE *f) {
	while (fgets(buff, FILE_BUFFER_SIZE - 1, f)) {
		myTrim(buff, buff);

		if (*buff == '[')
			break;

		if ((*buff) >= '0' && (*buff) <= '9' || (*buff) >= 'A' && (*buff) <= 'Z' || (*buff) >= 'a' && (*buff) <= 'z' || ((*buff) && strchr("/\\?*", (*buff)))) {
			return buff;
		}
	}

	fclose(f);
	return NULL;
}

char* myGetToken(char* buff, unsigned char index) {
	while (*buff) {
		if (index) {
			index--;
		} else {
			break;
		}
		buff += strlen(buff) + 1;
	}
	return buff;
}

unsigned short myTokenize(char *target, char *source, char *sep, bool whiteSep) {
	bool found = true;
	char *dp = target;
	unsigned short kount = 0;

	while (*source) {
		if (sep && sep[0] && strchr(sep, (*source))) {
			found = true;
			source++;
			continue;
		} else if (whiteSep && (*source) <= 32) {
			found = true;
			source++;
			continue;
		}

		if (found) {
			if (target != dp) {
				*dp = 0;
				dp++;
			}
			kount++;
		}

		found = false;
		*dp = *source;
		dp++;
		source++;
	}

	*dp = 0;
	dp++;
	*dp = 0;

	return kount;
}

char* myTrim(char *target, char *source) {
	while ((*source) && (*source) <= 32) {
		source++;
	}

	int i = 0;

	for (; i < 511 && source[i]; i++) {
		target[i] = source[i];
	}

	target[i] = source[i];
	i--;

	for (; i >= 0 && target[i] <= 32; i--) {
		target[i] = 0;
	}

	return target;
}

void mySplit(char *name, char *value, char *source, char splitChar) {
	int i = 0;
	int j = 0;
	int k = 0;

	for (; source[i] && j <= 510 && source[i] != splitChar; i++, j++) {
		name[j] = source[i];
	}

	if (source[i]) {
		i++;
		for (; k <= 510 && source[i]; i++, k++) {
			value[k] = source[i];
		}
	}

	name[j] = 0;
	value[k] = 0;

	myTrim(name, name);
	myTrim(value, value);
}

bool isIP(char *string) {
	int j = 0;

	for (; *string; string++) {
		if (*string == '.' && *(string + 1) != '.')
			j++;
		else if (*string < '0' || *string > '9')
			return 0;
	}

	if (j == 3) {
		return 1;
	} else {
		return 0;
	}
}

char *myUpper(char *string) {
	char diff = 'a' - 'A';
	size_t len = strlen(string);

	for (int i = 0; i < len; i++) {
		if (string[i] >= 'a' && string[i] <= 'z') {
			string[i] -= diff;
		}
	}
	return string;
}

char *myLower(char *string) {
	char diff = 'a' - 'A';
	size_t len = strlen(string);

	for (int i = 0; i < len; i++) {
		if (string[i] >= 'A' && string[i] <= 'Z') {
			string[i] += diff;
		}
	}
	return string;
}

void init(void *lpParam) {
	char moduleFileName[MAX_PATH];

	memset(&cfig, 0, sizeof(cfig));

	GetModuleFileName(NULL, moduleFileName, _MAX_PATH);
	char *fileExt = strrchr(moduleFileName, '.');
	FILE *f = NULL;
	char raw[512];
	char name[512];
	char value[512];

	*fileExt = 0;
	sprintf_s(iniFile, sizeof(iniFile), "%s.ini", moduleFileName);
	sprintf_s(lnkFile, sizeof(lnkFile), "%s.url", moduleFileName);
	fileExt = strrchr(moduleFileName, '\\');
	*fileExt = 0;
	fileExt++;
	sprintf_s(logFile, sizeof(logFile), "%s\\log\\%s%%Y%%m%%d.log", moduleFileName, fileExt);

	if (verbatim) {
		cfig.logLevel = 2;
	} else if (f = openSection("LOGGING", 1, iniFile)) {
		cfig.logLevel = 1;

		while (readSection(raw, f)) {
			if (!_stricmp(raw, "None")) {
				cfig.logLevel = 0;
			} else if (!_stricmp(raw, "Errors")) {
				cfig.logLevel = 1;
			} else if (!_stricmp(raw, "All")) {
				cfig.logLevel = 2;
			} else {
				cfig.logLevel = 1;
				logMess(1, "Section [LOGGING], Invalid LogLevel: %s, defaulting to Error", raw);
			}
		}
	}

	if (cfig.logLevel && logFile[0]) {
		time_t t = time(NULL);
		tm ttm;
		char logFileName[MAX_PATH];

		localtime_s(&ttm, &t);
		loggingDay = ttm.tm_yday;
		strftime(logFileName, sizeof(logFileName), logFile, &ttm);

		fopen_s(&cfig.logfile, logFileName, "at");

		if (cfig.logfile) {
			WritePrivateProfileString("InternetShortcut", "URL", logFileName, lnkFile);
			WritePrivateProfileString("InternetShortcut", "IconIndex", "0", lnkFile);
			WritePrivateProfileString("InternetShortcut", "IconFile", logFileName, lnkFile);
		}
	}

	unsigned short wVersionRequested = MAKEWORD(1, 1);
	WSAStartup(wVersionRequested, &cfig.wsaData);

	if (cfig.wsaData.wVersion != wVersionRequested) {
		logMess(1, "WSAStartup Error");
	}

	if (f = openSection("HOME", 1, iniFile)) {
		while (readSection(raw, f)) {
			mySplit(name, value, raw, '=');

			if (strlen(value)) {
				if (!cfig.homes[0].alias[0] && cfig.homes[0].target[0]) {
					logMess(1, "Section [HOME], alias and bare path mixup, entry %s ignored", raw);
				} else if (strchr(name, '/') || strchr(name, '\\') || strchr(name, '>') || strchr(name, '<') || strchr(name, '.')) {
					logMess(1, "Section [HOME], invalid chars in alias %s, entry ignored", name);
				} else if (name[0] && strlen(name) < 64 && value[0]) {
					for (int i = 0; i < 8; i++) {
						if (cfig.homes[i].alias[0] && !_stricmp(name, cfig.homes[i].alias)) {
							logMess(1, "Section [HOME], Duplicate Entry: %s ignored", raw);
							break;
						} else if (!cfig.homes[i].alias[0]) {
							strcpy_s(cfig.homes[i].alias, sizeof(cfig.homes[i].alias), name);
							strcpy_s(cfig.homes[i].target, sizeof(cfig.homes[i].target), value);

							if (cfig.homes[i].target[strlen(cfig.homes[i].target) - 1] != '\\') {
								strcat_s(cfig.homes[i].target, sizeof(cfig.homes[i].target), "\\");
							}
							break;
						}
					}
				} else {
					logMess(1, "Section [HOME], alias [%s] too large", name);
				}
			} else if (!cfig.homes[0].alias[0] && !cfig.homes[0].target[0]) {
				strcpy_s(cfig.homes[0].target, sizeof(cfig.homes[0].target), name);

				if (cfig.homes[0].target[strlen(cfig.homes[0].target) - 1] != '\\') {
					strcat_s(cfig.homes[0].target, sizeof(cfig.homes[0].target), "\\");
				}
			} else if (cfig.homes[0].alias[0]) {
				logMess(1, "Section [HOME], alias and bare path mixup, entry %s ignored", raw);
			} else if (cfig.homes[0].target[0]) {
				logMess(1, "Section [HOME], Duplicate Path: %s ignored", raw);
			} else {
				logMess(1, "Section [HOME], missing = sign, Invalid Entry: %s ignored", raw);
			}
		}
	}

	if (!cfig.homes[0].target[0]) {
		GetModuleFileName(NULL, cfig.homes[0].target, UCHAR_MAX);
		char *iniFileExt = strrchr(cfig.homes[0].target, '\\');
		*(++iniFileExt) = 0;
	}

	cfig.fileRead = true;

	if (f = openSection("TFTP-OPTIONS", 1, iniFile)) {
		while (readSection(raw, f)) {
			mySplit(name, value, raw, '=');

			if (strlen(value)) {
				if (!_stricmp(name, "blksize")) {
					unsigned int tblksize = atol(value);

					if (tblksize < 512)
						blksize = 512;
					else if (tblksize > USHRT_MAX - 32)
						blksize = USHRT_MAX - 32;
					else
						blksize = tblksize;
				} else if (!_stricmp(name, "threadpoolsize")) {
					minThreads = atoi(value);
					if (minThreads < 1)
						minThreads = 0;
					else if (minThreads > 100)
						minThreads = 100;
				} else if (!_stricmp(name, "timeout")) {
					timeout = atoi(value);
					if (timeout < 1)
						timeout = 1;
					else if (timeout > UCHAR_MAX)
						timeout = UCHAR_MAX;
				} else if (!_stricmp(name, "Read")) {
					if (strchr("Yy", *value)) {
						cfig.fileRead = true;
					} else {
						cfig.fileRead = false;
					}
				} else if (!_stricmp(name, "Write")) {
					if (strchr("Yy", *value)) {
						cfig.fileWrite = true;
					} else {
						cfig.fileWrite = false;
					}
				} else if (!_stricmp(name, "Overwrite")) {
					if (strchr("Yy", *value)) {
						cfig.fileOverwrite = true;
					} else {
						cfig.fileOverwrite = false;
					}
				} else if (!_stricmp(name, "port-range")) {
					char *ptr = strchr(value, '-');
					if (ptr) {
						*ptr = 0;
						cfig.minport = atol(value);
						cfig.maxport = atol(++ptr);

						if (cfig.minport < 1024 || cfig.minport >= USHRT_MAX || cfig.maxport < 1024 || cfig.maxport >= USHRT_MAX || cfig.minport > cfig.maxport) {
							cfig.minport = 0;
							cfig.maxport = 0;

							logMess(1, "Invalid port range %s", value);
						}
					} else {
						logMess(1, "Invalid port range %s", value);
					}
				} else {
					logMess(1, "Warning: unknown option %s, ignored", name);
				}
			}
		}
	}

	if (f = openSection("ALLOWED-CLIENTS", 1, iniFile)) {
		int i = 0;

		while (readSection(raw, f)) {
			if (i < 32) {
				unsigned int rs = 0;
				unsigned int re = 0;
				mySplit(name, value, raw, '-');
				InetPton(AF_INET, name, &rs);

				if (strlen(value)) {
					InetPton(AF_INET, name, &re);
				} else {
					re = rs;
				}

				if (rs && rs != INADDR_NONE && re && re != INADDR_NONE && rs <= re) {
					cfig.hostRanges[i].rangeStart = rs;
					cfig.hostRanges[i].rangeEnd = re;
					i++;
				} else {
					logMess(1, "Section [ALLOWED-CLIENTS] Invalid entry %s in ini file, ignored", raw);
				}
			}
		}
	}

	logMess(1, "");
	if (verbatim) {
		logMess(1, "%s (Interactive mode)", SERVICE_DISPLAY_NAME);

	} else {
		logMess(1, "%s (Service mode)", SERVICE_DISPLAY_NAME);
	}
	logMess(1, "Version: %s, build date: %s", SERVICE_VERSION, SERVICE_BUILD);
	logMess(1, "");

	logMess(1, "Configuration summary:");
	for (int i = 0; i < MAX_SERVERS; i++) {
		if (cfig.homes[i].target[0]) {
			logMess(1, "Alias /%s is mapped to %s", cfig.homes[i].alias, cfig.homes[i].target);
		}
	}

	if (cfig.hostRanges[0].rangeStart) {
		unsigned long ipStart;
		unsigned long ipEnd;
		PSTR rangeStart = NULL;
		PSTR rangeEnd = NULL;

		for (unsigned short i = 0; i <= sizeof(cfig.hostRanges) && cfig.hostRanges[i].rangeStart; i++) {
			ipStart = htonl(cfig.hostRanges[i].rangeStart);
			ipEnd = htonl(cfig.hostRanges[i].rangeEnd);
			rangeStart = ipv4_ntop(&ipStart);
			rangeEnd = ipv4_ntop(&ipEnd);

			logMess(1, "Permitted clients: %s-%s", rangeStart, rangeEnd);

			free(rangeStart);
			free(rangeEnd);
		}
	} else {
		logMess(1, "Permitted clients: all");
	}

	if (cfig.minport) {
		logMess(1, "Server port range: %u-%u", cfig.minport, cfig.maxport);
	} else {
		logMess(1, "Server port range: all");
	}

	logMess(1, "Max blksize: %u", blksize);
	logMess(1, "Default blksize: %u", 512);
	logMess(1, "Default timeout: %u", timeout);
	logMess(1, "File read allowed: %s", cfig.fileRead ? "Yes" : "No");
	logMess(1, "File create allowed: %s", cfig.fileWrite ? "Yes" : "No");
	logMess(1, "File overwrite allowed: %s", cfig.fileOverwrite ? "Yes" : "No");
	logMess(1, "Logging: %s", cfig.logLevel > 1 ? "all" : "errors");

	lEvent = CreateEvent(NULL, FALSE, TRUE, TEXT("AchalTFTServerLogEvent"));

	if (lEvent == NULL) {
		logMess(0, "CreateEvent error: %d\n", GetLastError());
		exit(-1);
	} else if (GetLastError() == ERROR_ALREADY_EXISTS) {
		logMess(0, "CreateEvent opened an existing Event\nServer May already be Running");
		exit(-1);
	}

	tEvent = CreateEvent(NULL, FALSE, FALSE, TEXT("AchalTFTServerThreadEvent"));

	if (tEvent == NULL) {
		logMess(0, "CreateEvent error: %d\n", GetLastError());
		exit(-1);
	} else if (GetLastError() == ERROR_ALREADY_EXISTS) {
		logMess(0, "CreateEvent opened an existing Event\nServer May already be Running");
		exit(-1);
	}

	sEvent = CreateEvent(NULL, FALSE, TRUE, TEXT("AchalTFTServerSocketEvent"));

	if (sEvent == NULL) {
		logMess(0, "CreateEvent error: %d\n", GetLastError());
		exit(-1);
	} else if (GetLastError() == ERROR_ALREADY_EXISTS) {
		logMess(0, "CreateEvent opened an existing Event\nServer May already be Running");
		exit(-1);
	}

	cEvent = CreateEvent(NULL, FALSE, TRUE, TEXT("AchalTFTServerCountEvent"));

	if (cEvent == NULL) {
		logMess(0, "CreateEvent error: %d\n", GetLastError());
		exit(-1);
	} else if (GetLastError() == ERROR_ALREADY_EXISTS) {
		logMess(0, "CreateEvent opened an existing Event\nServer May already be Running");
		exit(-1);
	}

	if (minThreads) {
		for (int i = 0; i < minThreads; i++) {
			_beginthread(processRequest, 0, NULL);
		}

		logMess(1, "Thread pool size: %u", minThreads);
	}

	for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].port; i++) {
		PSTR ip = ipv4_ntop(&network.tftpConn[i].server);

		logMess(1, "Listening on: %s:%i", ip, network.tftpConn[i].port);
		free(ip);
	}

	do {
		memset(&newNetwork, 0, sizeof(data1));

		bool ifSpecified = false;
		bool bindfailed = false;

		if (f = openSection("LISTEN-ON", 1, iniFile)) {
			unsigned char i = 0;

			while (readSection(raw, f)) {
				unsigned short port = 69;

				cfig.ifspecified = true;
				mySplit(name, value, raw, ':');

				if (value[0])
					port = atoi(value);

				if (i < MAX_SERVERS) {
					if (isIP(name)) {
						unsigned int addr;
						InetPton(AF_INET, name, &addr);

						if (!addr) {
							newNetwork.listenServers[0] = 0;
							newNetwork.listenPorts[0] = port;
							fclose(f);
							break;
						} else if (!findServer(newNetwork.listenServers, addr)) {
							newNetwork.listenServers[i] = addr;
							newNetwork.listenPorts[i] = port;
							i++;
						}
					} else {
						logMess(1, "Warning: Section [LISTEN-ON], Invalid Interface Address %s, ignored", raw);
					}
				}
			}
		}

		if (!cfig.ifspecified) {
			logMess(1, "Detecting Interfaces..");
			getInterfaces(&newNetwork);

			for (unsigned char n = 0; n < MAX_SERVERS && newNetwork.staticServers[n]; n++) {
				newNetwork.listenServers[n] = newNetwork.staticServers[n];
				newNetwork.listenPorts[n] = 69;
			}
		}

		unsigned char i = 0;

		for (int j = 0; j < MAX_SERVERS && newNetwork.listenPorts[j]; j++) {
			int k = 0;

			for (; k < MAX_SERVERS && network.tftpConn[k].loaded; k++) {
				if (network.tftpConn[k].ready && network.tftpConn[k].server == newNetwork.listenServers[j] && network.tftpConn[k].port == newNetwork.listenPorts[j]) {
					break;
				}

			}

			if (network.tftpConn[k].ready && network.tftpConn[k].server == newNetwork.listenServers[j] && network.tftpConn[k].port == newNetwork.listenPorts[j]) {
				memcpy(&(newNetwork.tftpConn[i]), &(network.tftpConn[k]), sizeof(tftpConnType));

				if (newNetwork.maxFD < newNetwork.tftpConn[i].sock) {
					newNetwork.maxFD = newNetwork.tftpConn[i].sock;
				}

				network.tftpConn[k].ready = false;
				i++;
				continue;
			} else {
				newNetwork.tftpConn[i].sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

				if (newNetwork.tftpConn[i].sock == INVALID_SOCKET) {
					bindfailed = true;
					logMess(1, "Failed to Create Socket");
					continue;
				}

				errno = 0;
				newNetwork.tftpConn[i].addr.sin_family = AF_INET;
				newNetwork.tftpConn[i].addr.sin_addr.s_addr = newNetwork.listenServers[j];
				newNetwork.tftpConn[i].addr.sin_port = htons(newNetwork.listenPorts[j]);
				int nRet = bind(newNetwork.tftpConn[i].sock, (sockaddr*)&newNetwork.tftpConn[i].addr, sizeof(struct sockaddr_in));

				if (nRet == SOCKET_ERROR || errno) {
					PSTR tmpIp = ipv4_ntop(&newNetwork.listenServers[j]);

					bindfailed = true;
					closesocket(newNetwork.tftpConn[i].sock);
					logMess(1, "%s Port %i bind failed", tmpIp, newNetwork.listenPorts[j]);
					free(tmpIp);
					continue;
				}

				newNetwork.tftpConn[i].loaded = true;
				newNetwork.tftpConn[i].ready = true;
				newNetwork.tftpConn[i].server = newNetwork.listenServers[j];
				newNetwork.tftpConn[i].port = newNetwork.listenPorts[j];

				if (newNetwork.maxFD < newNetwork.tftpConn[i].sock) {
					newNetwork.maxFD = newNetwork.tftpConn[i].sock;
				}

				if (!newNetwork.listenServers[j]) {
					break;
				}

				i++;
			}
		}

		if (bindfailed) {
			cfig.failureCount++;
		} else {
			cfig.failureCount = 0;
		}

		closeConn();
		memcpy(&network, &newNetwork, sizeof(data1));

		if (!network.tftpConn[0].ready) {
			logMess(1, "No Static Interface ready, Waiting...");
			continue;
		}

		for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].loaded; i++) {
			PSTR tmpIp = ipv4_ntop(&network.tftpConn[i].server);
			logMess(1, "Listening On: %s:%d", tmpIp, network.tftpConn[i].port);
			free(tmpIp);
		}

		network.ready = true;

	} while (detectChange());

	_endthread();
	return;
}

bool detectChange() {
	if (!cfig.failureCount) {
		if (cfig.ifspecified) {
			return false;
		}
	}

	unsigned int eventWait = UINT_MAX;

	if (cfig.failureCount) {
		eventWait = 10000 * (int)pow(2, cfig.failureCount);
	}

	OVERLAPPED overlap;
	unsigned int ret;
	HANDLE hand = NULL;
	overlap.hEvent = WSACreateEvent();

	ret = NotifyAddrChange(&hand, &overlap);

	if (ret != NO_ERROR) {
		if (WSAGetLastError() != WSA_IO_PENDING) {
			logMess(1, "NotifyAddrChange error...%d\n", WSAGetLastError());
			return true;
		}
	}

	if (WaitForSingleObject(overlap.hEvent, eventWait) == WAIT_OBJECT_0) {
		WSACloseEvent(overlap.hEvent);
	}

	network.ready = false;

	while (network.busy)
		Sleep(1000);

	if (cfig.failureCount) {
		logMess(1, "Retrying failed Listening Interfaces..");
	} else {
		logMess(1, "Network changed, re-detecting Interfaces..");

	}

	return true;
}

void getInterfaces(data1 *network) {
	memset(network, 0, sizeof(data1));

	SOCKET sd = WSASocket(PF_INET, SOCK_DGRAM, 0, 0, 0, 0);

	if (sd == INVALID_SOCKET) {
		return;
	}

	INTERFACE_INFO InterfaceList[MAX_SERVERS];
	unsigned long nBytesReturned;

	if (WSAIoctl(sd, SIO_GET_INTERFACE_LIST, 0, 0, &InterfaceList, sizeof(InterfaceList), &nBytesReturned, 0, 0) == SOCKET_ERROR) {
		return;
	}

	int nNumInterfaces = nBytesReturned / sizeof(INTERFACE_INFO);

	for (int i = 0; i < nNumInterfaces; ++i) {
		sockaddr_in *pAddress = (sockaddr_in*)&(InterfaceList[i].iiAddress);
		u_long nFlags = InterfaceList[i].iiFlags;

		if (pAddress->sin_addr.s_addr) {
			addServer(network->allServers, pAddress->sin_addr.s_addr);

			if (!(nFlags & IFF_POINTTOPOINT) && (nFlags & IFF_UP)) {
				addServer(network->staticServers, pAddress->sin_addr.s_addr);
			}
		}
	}

	closesocket(sd);
}


bool addServer(unsigned int *array, unsigned int ip) {
	for (unsigned char i = 0; i < MAX_SERVERS; i++) {
		if (!ip || array[i] == ip) {
			return 0;
		} else if (!array[i]) {
			array[i] = ip;
			return 1;
		}
	}
	return 0;
}

unsigned int *findServer(unsigned int *array, unsigned int ip) {
	if (ip) {
		for (unsigned char i = 0; i < MAX_SERVERS && array[i]; i++) {
			if (array[i] == ip) {
				return &(array[i]);
			}
		}
	}
	return 0;
}

PSTR vsnprintf_malloc(const char *fmt, va_list ap) {
	va_list apl;
	int size = 0;
	PVOID p = NULL;

	va_copy(apl, ap);
	size = vsnprintf((PSTR)p, size, fmt, apl);
	va_end(apl);

	if (size < 0) {
		return(NULL);
	}

	size++;

	p = calloc(size, sizeof(char));
	if (p == NULL) {
		return(NULL);
	}

	va_copy(apl, ap);
	size = vsnprintf((PSTR)p, size, fmt, apl);
	va_end(apl);

	if (size < 0) {
		free(p);
		return(NULL);
	}

	return (PSTR)p;
}

PSTR ipv4_ntop(const void *pAddr) {
	PSTR pStringBuf = (PSTR)calloc(INET_ADDRSTRLEN, sizeof(char));

	inet_ntop(AF_INET, pAddr, pStringBuf, (size_t)INET_ADDRSTRLEN);

	return(pStringBuf);
}

void logMess(unsigned char logLevel, const char *fmt, ...) {
	PSTR logBuff = NULL;
	va_list ap;

	WaitForSingleObject(lEvent, INFINITE);

	va_start(ap, fmt);
	logBuff = vsnprintf_malloc(fmt, ap);
	va_end(ap);

	if (verbatim) {
		printf("%s\n", logBuff);
	}
	//else if (cfig.logfile && logLevel <= cfig.logLevel)
	if (cfig.logfile && logLevel <= cfig.logLevel) {
		time_t t = time(NULL);
		tm ttm;
		localtime_s(&ttm, &t);
		char timeStamp[64];

		if (ttm.tm_yday != loggingDay) {
			char logFileName[MAX_PATH];

			loggingDay = ttm.tm_yday;
			strftime(logFileName, sizeof(logFileName), logFile, &ttm);
			fprintf(cfig.logfile, "Logging Continued on file %s\n", logFileName);
			fclose(cfig.logfile);
			fopen_s(&cfig.logfile, logFileName, "at");

			if (cfig.logfile) {
				fprintf(cfig.logfile, "%s version %s\n\n", SERVICE_DISPLAY_NAME, SERVICE_VERSION);
				WritePrivateProfileString("InternetShortcut", "URL", logFileName, lnkFile);
				WritePrivateProfileString("InternetShortcut", "IconIndex", "0", lnkFile);
				WritePrivateProfileString("InternetShortcut", "IconFile", logFileName, lnkFile);
			} else
				return;
		}

		strftime(timeStamp, sizeof(timeStamp), "%Y-%m-%d %X", &ttm);
		fprintf(cfig.logfile, "[%s] %s\n", timeStamp, logBuff);
		fflush(cfig.logfile);
		free(logBuff);
	}
	SetEvent(lEvent);
}

void logMess(unsigned char logLevel, request *req, const char *fmt, ...) {
	PSTR logBuff = NULL;
	va_list ap;

	WaitForSingleObject(lEvent, INFINITE);

	va_start(ap, fmt);
	logBuff = vsnprintf_malloc(fmt, ap);
	va_end(ap);

	strncpy_s(req->serverError.errormessage, logBuff, sizeof(req->serverError.errormessage) - 1);

	if (verbatim) {
		PSTR tmpIp = ipv4_ntop(&req->client.sin_addr.s_addr);

		if (!req->serverError.errormessage[0])
			strerror_s(req->serverError.errormessage, sizeof(req->serverError.errormessage), errno);

		if (req->path[0])
			printf("Client %s:%u %s, %s\n", tmpIp, ntohs(req->client.sin_port), req->path, req->serverError.errormessage);
		else
			printf("Client %s:%u, %s\n", tmpIp, ntohs(req->client.sin_port), req->serverError.errormessage);
		free(tmpIp);
	}
	//else if (cfig.logfile && logLevel <= cfig.logLevel)
	if (cfig.logfile && logLevel <= cfig.logLevel) {
		time_t t = time(NULL);
		tm ttm;
		PSTR tmpIp = ipv4_ntop(&req->client.sin_addr.s_addr);
		char timeStamp[64];

		localtime_s(&ttm, &t);

		if (ttm.tm_yday != loggingDay) {
			char logFileName[MAX_PATH];

			loggingDay = ttm.tm_yday;
			strftime(logFileName, sizeof(logFileName), logFile, &ttm);
			fprintf(cfig.logfile, "Logging Continued on file %s\n", logFileName);
			fclose(cfig.logfile);
			fopen_s(&cfig.logfile, logFileName, "at");

			if (cfig.logfile) {
				fprintf(cfig.logfile, "%s version %s\n\n", SERVICE_DISPLAY_NAME, SERVICE_VERSION);
				WritePrivateProfileString("InternetShortcut", "URL", logFileName, lnkFile);
				WritePrivateProfileString("InternetShortcut", "IconIndex", "0", lnkFile);
				WritePrivateProfileString("InternetShortcut", "IconFile", logFileName, lnkFile);
			} else
				return;
		}

		strftime(timeStamp, sizeof(timeStamp), "%Y-%m-%d %X", &ttm);

		if (req->path[0]) {
			fprintf(cfig.logfile, "[%s] Client %s:%u %s, %s\n", timeStamp, tmpIp, ntohs(req->client.sin_port), req->path, req->serverError.errormessage);
		} else {
			fprintf(cfig.logfile, "[%s] Client %s:%u, %s\n", timeStamp, tmpIp, ntohs(req->client.sin_port), req->serverError.errormessage);
		}

		fflush(cfig.logfile);

		free(tmpIp);
	}
	SetEvent(lEvent);
}
