// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaNetworkBackendTcp.h"
#include "UbaConfig.h"
#include "UbaEvent.h"
#include "UbaHash.h"
#include "UbaPlatform.h"
#include "UbaStringBuffer.h"
#include "UbaTimer.h"

#if PLATFORM_WINDOWS
#include <iphlpapi.h>
#include <ipifcons.h>
#include <Mstcpip.h>
#pragma comment (lib, "Netapi32.lib")
#pragma comment (lib, "Ws2_32.lib")
#pragma comment(lib, "IPHLPAPI.lib") // For GetAdaptersInfo
#else
#include <netdb.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <poll.h>
#define TIMEVAL timeval
#define SOCKET_ERROR -1
#define SOCKET int
#define INVALID_SOCKET -1
#define SD_BOTH SHUT_RDWR
#define WSAHOST_NOT_FOUND 0
#define WSAENOTCONN ENOTCONN
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEADDRINUSE EADDRINUSE
#define closesocket(a) close(a)
#define addrinfoW addrinfo
#define GetAddrInfoW getaddrinfo
#define FreeAddrInfoW freeaddrinfo
#define WSAGetLastError() errno
#define strcpy_s(a, b, c) strcpy(a, c)
#define WSAPOLLFD pollfd
#define WSAPoll poll
#endif

#define UBA_LOG_SOCKET_ERRORS UBA_DEBUG
#define UBA_EMULATE_BAD_INTERNET 0

namespace uba
{
	constexpr u32 MaxHeaderSize = 24;

	struct NetworkBackendTcp::ListenEntry
	{
		StringBuffer<128> ip;
		u16 port;
		ListenConnectedFunc connectedFunc;
		Event listening;
		Atomic<SOCKET> socket = INVALID_SOCKET;
		Thread thread;
	};

	struct NetworkBackendTcp::Connection
	{
		Connection(Logger& l, SOCKET s) : logger(l), socket(s), ready(true) { CreateGuid(uid); }

		Logger& logger;
		Atomic<SOCKET> socket;

		Event ready;
		Guid uid;
		u32 headerSize = 0;
		
		u32 recvTimeoutMs = 0;
		void* recvTimeoutContext = nullptr;
		RecvTimeoutCallback* recvTimeoutCallback = nullptr;

		void* recvContext = nullptr;
		RecvHeaderCallback* headerCallback = nullptr;
		RecvBodyCallback* bodyCallback = nullptr;
		const tchar* recvHint = TC("");

		void* dataSentContext = nullptr;
		DataSentCallback* dataSentCallback = nullptr;

		void* disconnectContext = nullptr;
		DisconnectCallback* disconnectCallback = nullptr;

		Futex sendLock;
		CriticalSection shutdownLock;

		Thread recvThread;

		bool allowLess = false;

		Connection(const Connection&) = delete;
		void operator=(const Connection&) = delete;
	};

	bool SetKeepAlive(Logger& logger, SOCKET socket);
	bool SetBlocking(Logger& logger, SOCKET socket, bool blocking);
	bool SetTimeout(Logger& logger, SOCKET socket, u32 timeoutMs);
	bool SetLinger(Logger& logger, SOCKET socket, u32 lingerSeconds);
	bool SetRecvBuf(Logger& logger, SOCKET socket, u32 windowSize);
	bool SetSendBuf(Logger& logger, SOCKET socket, u32 windowSize);
	bool DisableNagle(Logger& logger, SOCKET socket);

	struct NetworkBackendTcp::RecvCache
	{
		u8 bytes[128*1024];
		u32 byteCount = 0;
	};

	bool NetworkBackendTcp::EnsureInitialized(Logger& logger)
	{
		#if PLATFORM_WINDOWS
		WSADATA wsaData;
		if (!m_wsaInitDone)
			if (int res = WSAStartup(MAKEWORD(2, 2), &wsaData))
				return logger.Error(TC("WSAStartup failed (%d)"), res);
		m_wsaInitDone = true;
		#else
		static bool initOnce = []()
			{
				struct sigaction sa = { { SIG_IGN } };
				sigaction(SIGPIPE, &sa, NULL); // Needed for broken pipe that can happen if helpers crash
				return true;
			}();

		#endif
		return true;
	}

	bool ShutdownSocket(Logger& logger, SOCKET s, const tchar* hint)
	{
		if (s == INVALID_SOCKET)
			return true;
		if (shutdown(s, SD_BOTH) != SOCKET_ERROR)
			return true;
		if (WSAGetLastError() == WSAENOTCONN)
			return true;
		logger.Info(TC("Failed to shutdown socket %llu in %s (%s)"), u64(s), hint, LastErrorToText(WSAGetLastError()).data);
		return false;
	}

	bool CloseSocket(Logger& logger, SOCKET s, const tchar* hint)
	{
		if (s == INVALID_SOCKET)
			return true;
		if (closesocket(s) != SOCKET_ERROR)
			return true;
		logger.Info(TC("Failed to close socket %llu in %s (%s)"), u64(s), hint, LastErrorToText(WSAGetLastError()).data);
		return false;
	}

	void NetworkBackendTcpCreateInfo::Apply(Config& config, const tchar* tableName)
	{
		const ConfigTable* tablePtr = config.GetTable(tableName);
		if (!tablePtr)
			return;
		const ConfigTable& table = *tablePtr;
		table.GetValueAsBool(disableNagle, TC("DisableNagle"));
	}

	NetworkBackendTcp::NetworkBackendTcp(const NetworkBackendTcpCreateInfo& info, const tchar* prefix)
		: m_logger(info.logWriter, prefix)
	{
		m_disableNagle = info.disableNagle;
	}

	NetworkBackendTcp::~NetworkBackendTcp()
	{
		StopListen();

		SCOPED_FUTEX(m_connectionsLock, lock);
		for (auto& conn : m_connections)
		{
			SCOPED_CRITICAL_SECTION(conn.shutdownLock, lock2);
			if (conn.socket == INVALID_SOCKET)
				continue;
			SOCKET s = conn.socket;
			conn.socket = INVALID_SOCKET;
			ShutdownSocket(conn.logger, s, TC("Dtor"));
			lock2.Leave();
			conn.recvThread.Wait();
			CloseSocket(conn.logger, s, TC("Dtor"));
		}
		m_connections.clear();

		#if PLATFORM_WINDOWS
		if (m_wsaInitDone)
			WSACleanup();
		#endif
	}

	void NetworkBackendTcp::Shutdown(void* connection)
	{
		auto& conn = *(Connection*)connection;
		SCOPED_CRITICAL_SECTION(conn.shutdownLock, lock);
		if (conn.socket == INVALID_SOCKET)
			return;
		ShutdownSocket(conn.logger, conn.socket, TC("Shutdown"));
	}

	bool NetworkBackendTcp::Send(Logger& logger, void* connection, const void* data, u32 dataSize, SendContext& sendContext, const tchar* sendHint)
	{
		auto& conn = *(Connection*)connection;
		sendContext.isUsed = true;

		SCOPED_FUTEX(conn.sendLock, lock);
		if (conn.socket == INVALID_SOCKET)
			return false;
		bool res = SendSocket(conn, logger, data, dataSize, sendHint);
		lock.Leave();

		sendContext.isFinished = true;

		m_totalSend += dataSize;

		if (auto c = conn.dataSentCallback)
			c(conn.dataSentContext, dataSize);
		return res;
	}

	void NetworkBackendTcp::SetDataSentCallback(void* connection, void* context, DataSentCallback* callback)
	{
		auto& conn = *(Connection*)connection;
		conn.dataSentCallback = callback;
		conn.dataSentContext = context;
	}

	void NetworkBackendTcp::SetRecvCallbacks(void* connection, void* context, u32 headerSize, RecvHeaderCallback* h, RecvBodyCallback* b, const tchar* recvHint)
	{
		UBA_ASSERT(h);
		UBA_ASSERT(headerSize <= MaxHeaderSize);
		auto& conn = *(Connection*)connection;

		SCOPED_CRITICAL_SECTION(conn.shutdownLock, lock);
		UBA_ASSERTF(conn.disconnectCallback, TC("SetDisconnectCallback must be called before SetRecvCallbacks"));
		conn.recvContext = context;
		conn.headerSize = headerSize;
		conn.headerCallback = h;
		conn.bodyCallback = b;
		conn.recvHint = recvHint;
		conn.ready.Set();
	}

	void NetworkBackendTcp::SetRecvTimeout(void* connection, u32 timeoutMs, void* context, RecvTimeoutCallback* callback)
	{
		auto& conn = *(Connection*)connection;
		conn.recvTimeoutMs = timeoutMs;
		conn.recvTimeoutContext = context;
		conn.recvTimeoutCallback = callback;
	}

	void NetworkBackendTcp::SetDisconnectCallback(void* connection, void* context, DisconnectCallback* callback)
	{
		auto& conn = *(Connection*)connection;
		SCOPED_CRITICAL_SECTION(conn.shutdownLock, lock);
		conn.disconnectCallback = callback;
		conn.disconnectContext = context;
	}

	void NetworkBackendTcp::SetAllowLessThanBodySize(void* connection, bool allow)
	{
		auto& conn = *(Connection*)connection;
		SCOPED_CRITICAL_SECTION(conn.shutdownLock, lock);
		conn.allowLess = allow;
	}

	bool NetworkBackendTcp::StartListen(Logger& logger, u16 port, const tchar* ip, const ListenConnectedFunc& connectedFunc)
	{
		if (!EnsureInitialized(logger))
			return false;

		SCOPED_FUTEX(m_listenEntriesLock, lock);

		auto prevListenEntryCount = int(m_listenEntries.size());

		auto AddAddr = [&](const tchar* addr)
			{
				m_listenEntries.emplace_back();
				auto& entry = m_listenEntries.back();
				entry.ip.Append(addr);
				entry.port = port;
				entry.connectedFunc = connectedFunc;
			};

		if (ip && *ip)
		{
			AddAddr(ip);
		}
		else
		{
			TraverseNetworkAddresses(logger, [&](const StringBufferBase& addr)
				{
					AddAddr(addr.data);
					return true;
				});
			AddAddr(TC("127.0.0.1"));
		}

		if (m_listenEntries.empty())
		{
			logger.Warning(TC("No host addresses found for UbaServer. Will not be able to use remote workers"));
			return false;
		}

		auto skipCount = prevListenEntryCount;
		for (auto& e : m_listenEntries)
		{
			if (skipCount-- > 0)
				continue;
			e.listening.Create(true);
			e.thread.Start([this, &logger, &e]
				{
					ThreadListen(logger, e);
					return 0;
				}, TC("UbaTcpListen"));
		}

		bool success = true;
		skipCount = prevListenEntryCount;
		for (auto& e : m_listenEntries)
		{
			if (skipCount-- > 0)
				continue;
			if (!e.listening.IsSet(4000))
				success = false;
			if (e.socket == INVALID_SOCKET)
				success = false;
			e.listening.Destroy();
		}
		return success;
	}

	void NetworkBackendTcp::StopListen()
	{
		SCOPED_FUTEX(m_listenEntriesLock, lock);
		for (auto& e : m_listenEntries)
			e.socket = INVALID_SOCKET;
		for (auto& e : m_listenEntries)
			e.thread.Wait();
		m_listenEntries.clear();
	}

	bool NetworkBackendTcp::ThreadListen(Logger& logger, ListenEntry& entry)
	{
		addrinfoW hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET; //AF_UNSPEC; (Skip AF_INET6)
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_PASSIVE;

		// Resolve the server address and port
		struct addrinfoW* result = NULL;
		StringBuffer<32> portStr;
		portStr.AppendValue(entry.port);
		int res = GetAddrInfoW(entry.ip.data, portStr.data, &hints, &result);

		auto listenEv = MakeGuard([&]() { entry.listening.Set(); });

		if (res != 0)
			return logger.Error(TC("getaddrinfo failed (%d)"), res);

		UBA_ASSERT(result);
		auto addrGuard = MakeGuard([result]() { FreeAddrInfoW(result); });

		// Create a socket for listening to connections
		SOCKET listenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
		if (listenSocket == INVALID_SOCKET)
			return logger.Error(TC("socket failed (%s)"), LastErrorToText(WSAGetLastError()).data);

		auto listenSocketCleanup = MakeGuard([&]() { CloseSocket(logger, listenSocket, TC("listen cleanup")); });

		u32 reuseAddr = 1;
		if (::setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof reuseAddr) == SOCKET_ERROR)
			return logger.Error(TC("setsockopt SO_REUSEADDR failed (error: %s)"), LastErrorToText(WSAGetLastError()).data);

		// Setup the TCP listening socket
		res = bind(listenSocket, result->ai_addr, (socklen_t)result->ai_addrlen);

		if (res == SOCKET_ERROR)
		{
			int lastError = WSAGetLastError();
			if (lastError != WSAEADDRINUSE)
				return logger.Error(TC("bind %s:%hu failed (%s)"), entry.ip.data, entry.port, LastErrorToText(lastError).data);
			logger.Info(TC("bind %s:%hu failed because address/port is in use. Some other process is already using this address/port"), entry.ip.data, entry.port);
			return false;
		}

		addrGuard.Execute();

		res = listen(listenSocket, SOMAXCONN);
		if (res == SOCKET_ERROR)
			return logger.Error(TC("Listen failed (%s)"), LastErrorToText(WSAGetLastError()).data);

		if (!SetKeepAlive(logger, listenSocket))
			return false;

		logger.Info(TC("Listening on %s:%hu"), entry.ip.data, entry.port);
		entry.socket = listenSocket;

		listenEv.Execute();

		while (entry.socket != INVALID_SOCKET)
		{
			WSAPOLLFD p;
			p.fd = listenSocket;
			p.revents = 0;
			p.events = POLLIN;
			int timeoutMs = 2000;
			int pollRes = WSAPoll(&p, 1, timeoutMs);

			if (pollRes == SOCKET_ERROR)
			{
				int lastError = WSAGetLastError();
				logger.Warning(TC("WSAPoll returned error %s"), LastErrorToText(lastError).data);
				break;
			}

			if (!pollRes)
				continue;

			if (p.revents & POLLNVAL)
			{
				logger.Warning(TC("WSAPoll returned successful but with unexpected flags: %u"), p.revents);
				continue;
			}

			sockaddr remoteSockAddr = { 0 }; // for TCP/IP
			socklen_t remoteSockAddrLen = sizeof(remoteSockAddr);
			SOCKET clientSocket = accept(listenSocket, (sockaddr*)&remoteSockAddr, &remoteSockAddrLen);

			if (clientSocket == INVALID_SOCKET)
			{
				if (entry.socket != INVALID_SOCKET)
					logger.Info(TC("Accept failed with WSA error: %s"), LastErrorToText(WSAGetLastError()).data);
				break;
			}

			if (m_disableNagle && !DisableNagle(logger, clientSocket))
			{
				CloseSocket(logger, clientSocket, TC("disable nagle"));
				continue;
			}

			if (!SetKeepAlive(logger, clientSocket))
			{
				CloseSocket(logger, clientSocket, TC("keep alive"));
				continue;
			}

			SetLinger(logger, clientSocket, 10);

			// Hard coding this values disable auto-tuning. Seems like creating multiple sockets is more efficient than playing around with these
			//SetRecvBuf(logger, clientSocket, 50*1024*1024);
			//SetSendBuf(logger, clientSocket, 50*1024*1024);

			SCOPED_FUTEX(m_connectionsLock, lock);
			auto it = m_connections.emplace(m_connections.end(), logger, clientSocket);
			auto& conn = *it;
			conn.recvThread.Start([this, connPtr = &conn] { ThreadRecv(*connPtr); return 0; }, TC("UbaTcpRecv"));
			lock.Leave();

			if (!entry.connectedFunc(&conn, remoteSockAddr))
			{
				ShutdownSocket(logger, clientSocket, TC("ThreadListen"));
				conn.ready.Set();
				conn.recvThread.Wait();
				SCOPED_FUTEX(m_connectionsLock, lock2);
				m_connections.erase(it);
				continue;
			}
		}

		return true;
	}

	void NetworkBackendTcp::ThreadRecv(Connection& connection)
	{
		ElevateCurrentThreadPriority();
		
		auto& logger = connection.logger;

		if (connection.ready.IsSet(60000)) // This should never time out!
		{
			SetBlocking(logger, connection.socket, false);

			RecvCache recvCache;

			bool isFirst = true;
			while (connection.socket != INVALID_SOCKET)
			{
				void* bodyContext = nullptr;
				u8* bodyData = nullptr;
				u32 bodySize = 0;

				u8 headerData[MaxHeaderSize];
				if (!RecvSocket(connection, recvCache, headerData, connection.headerSize, TC(""), isFirst, false))
					break;
				isFirst = false;

				m_totalRecv += connection.headerSize;

				auto hc = connection.headerCallback;
				if (!hc)
				{
					logger.Error(TC("Tcp connection header callback not set"));
					break;
				}

				if (!hc(connection.recvContext, connection.uid, headerData, bodyContext, bodyData, bodySize))
					break;
				if (!bodySize)
					continue;

				bool success = RecvSocket(connection, recvCache, bodyData, bodySize, TC("Body"), false, connection.allowLess);

				m_totalRecv += bodySize;

				auto bc = connection.bodyCallback;
				if (!bc)
				{
					logger.Error(TC("Tcp connection body callback not set"));
					break;
				}

				if (!bc(connection.recvContext, !success, headerData, bodyContext, bodyData, bodySize))
					break;
				if (!success)
					break;
			}
		}
		else
		{
			logger.Warning(TC("Tcp connection timed out waiting for recv thread to be ready"));
		}

		SCOPED_CRITICAL_SECTION(connection.shutdownLock, lock2);
		SOCKET s = connection.socket;

		{
			SCOPED_FUTEX(connection.sendLock, lock);
			connection.socket = INVALID_SOCKET;
		}
		if (auto cb = connection.disconnectCallback)
		{
			auto context = connection.disconnectContext;
			connection.disconnectCallback = nullptr;
			connection.disconnectContext = nullptr;
			cb(context, connection.uid, &connection);
		}

		if (s == INVALID_SOCKET)
			return;
		ShutdownSocket(logger, s, TC("ThreadRecv"));
		CloseSocket(logger, s, TC("ThreadRecv"));
	}

	bool NetworkBackendTcp::Connect(Logger& logger, const tchar* ip, const ConnectedFunc& connectedFunc, u16 port, bool* timedOut)
	{
		if (!EnsureInitialized(logger))
			return false;

		u64 startTime = GetTime();

		if (timedOut)
			*timedOut = false;

		bool connected = false;
		bool success = true;
		TraverseRemoteAddresses(logger, ip, port, [&](const sockaddr& remoteSockaddr)
			{
				bool timedOut2 = false;
				connected = Connect(logger, remoteSockaddr, connectedFunc, &timedOut2, ip);
				if (connected)
					return false;
				if (timedOut2)
					return true;
				success = false;
				return false;
			});

		if (connected)
			return true;

		if (!success)
			return false;

		if (!timedOut)
			return false;

		*timedOut = true;
		int connectTimeMs = int(TimeToMs(GetTime() - startTime));
		int timeoutMs = 2000;
		if (connectTimeMs < timeoutMs)
			Sleep(u32(timeoutMs - connectTimeMs));
		return false;
	}

	bool NetworkBackendTcp::Connect(Logger& logger, const sockaddr& remoteSocketAddr, const ConnectedFunc& connectedFunc, bool* timedOut, const tchar* nameHint)
	{
		// Create a socket for connecting to server

		//TODO: Wrap this up in a better function
#if PLATFORM_WINDOWS
		SOCKET socketFd = WSASocketW(remoteSocketAddr.sa_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
		SOCKET socketFd = socket(remoteSocketAddr.sa_family, SOCK_STREAM, IPPROTO_TCP);
#endif
		if (socketFd == INVALID_SOCKET)
			return logger.Error(TC("socket failed (%s)"), LastErrorToText(WSAGetLastError()).data);

		// Create guard in case we fail to connect (will be cancelled further down if we succeed)
		auto socketClose = MakeGuard([&]() { CloseSocket(logger, socketFd, TC("Connect")); });

		// Set to non-blocking just for the connect call (we want to control the connect timeout after connect using select instead)
		if (!SetBlocking(logger, socketFd, false))
			return false;

		// Connect to server.
		int res = ::connect(socketFd, &remoteSocketAddr, sizeof(remoteSocketAddr));

#if PLATFORM_WINDOWS
		if (res == SOCKET_ERROR)
			if (WSAGetLastError() != WSAEWOULDBLOCK)
				return false;
#else
		if (res != 0)
		{
			if (errno != EINPROGRESS)
			{
				logger.Error(TC("Connect failed (%d: %s)"), WSAGetLastError(), LastErrorToText(WSAGetLastError()).data);
				return false;
			}
		}
#endif

		int timeoutMs = 2000;
		if (nameHint && (Equals(nameHint, TC("localhost")) || Equals(nameHint, TC("127.0.0.1"))))
			timeoutMs = 100;

		WSAPOLLFD p;
		p.fd = socketFd;
		p.revents = 0;
		p.events = POLLOUT;
		int pollRes = WSAPoll(&p, 1, timeoutMs);

		if (pollRes == SOCKET_ERROR)
		{
			int lastError = WSAGetLastError();
			logger.Warning(TC("WSAPoll returned error %s (%s)"), LastErrorToText(lastError).data, nameHint);
			return false;
		}

		u16 validFlags = POLLERR | POLLHUP; // Treat hangup as timeout (since we want retry if that happens). Also treat error as timeout. This is needed for Wine agent to be able to retry
		if (!pollRes || p.revents & validFlags)
		{
			if (timedOut)
				*timedOut = true;
			return false;
		}

		if (p.revents & POLLNVAL)
		{
			logger.Warning(TC("WSAPoll returned successful but with unexpected flags: %u"), p.revents);
			return false;
		}

		// Return to blocking since we want select to block
		if (!SetBlocking(logger, socketFd, true))
			return false;

#if !PLATFORM_WINDOWS
		// Before we send anything even though the
		// the socket is writable, but let's make sure
		// the connection is actually valid by getting
		// information about what we've connected to
		struct sockaddr_in junk;
		socklen_t length = sizeof(junk);
		memset(&junk, 0, sizeof(junk));
		if (getpeername(socketFd, (struct sockaddr *)&junk, &length) != 0)
		{
			if (timedOut)
				*timedOut = true;
			return false;
		}

		int sent = (int)send(socketFd, nullptr, 0, 0);
		if (sent == SOCKET_ERROR)
		{
			if (errno == ECONNREFUSED || errno == EPIPE)
			{
				if (timedOut)
					*timedOut = true;
				return false;
			}
			return false;
		}
#endif

		if (m_disableNagle && !DisableNagle(logger, socketFd))
			return false;

		if (!SetKeepAlive(logger, socketFd))
			return false;

		SetLinger(logger, socketFd, 10);

		// Hard coding this values disable auto-tuning. Seems like creating multiple sockets is more efficient than playing around with these
		//SetRecvBuf(logger, socketFd, 50*1024*1024);
		//SetSendBuf(logger, socketFd, 50*1024*1024);

		// Socket is good, cancel the socket close scope and break out of the loop.
		socketClose.Cancel();

		SCOPED_FUTEX(m_connectionsLock, lock);
		auto it = m_connections.emplace(m_connections.end(), logger, socketFd);
		auto& conn = *it;
		conn.recvThread.Start([this, connPtr = &conn] { ThreadRecv(*connPtr); return 0; }, TC("UbaTcpRecv"));
		lock.Leave();

		if (!connectedFunc(&conn, remoteSocketAddr, timedOut))
		{
			ShutdownSocket(logger, conn.socket, TC("Connect"));
			conn.ready.Set();
			conn.recvThread.Wait();
			SCOPED_FUTEX(m_connectionsLock, lock2);
			m_connections.erase(it);
			return false;
		}

		//char* ip = inet_ntoa(((sockaddr_in*)const_cast<sockaddr*>(&remoteSocketAddr))->sin_addr);
		if (nameHint)
			logger.Detail(TC("Connected to %s:%u (%s)"), nameHint, ((sockaddr_in&)remoteSocketAddr).sin_port, GuidToString(conn.uid).str);
		else
			logger.Detail(TC("Connected using sockaddr (%s)"), GuidToString(conn.uid).str);

		return true;
	}

	void NetworkBackendTcp::DeleteConnection(void* connection)
	{
		SCOPED_FUTEX(m_connectionsLock, lock);
		for (auto it=m_connections.begin();it!=m_connections.end(); ++it)
		{
			Connection& c = *it;
			if (&c != connection)
				continue;
			it = m_connections.erase(it);
			break;
		}
	}

	void NetworkBackendTcp::GetTotalSendAndRecv(u64& outSend, u64& outRecv)
	{
		outSend = m_totalSend;
		outRecv = m_totalRecv;
	}

	void NetworkBackendTcp::Validate(Logger& logger)
	{
		logger.Info(TC("  NetworkBackendTcp"));
		logger.Info(TC("     NoInfo"));
	}

	bool SetBlocking(Logger& logger, SOCKET socket, bool blocking)
	{
#if PLATFORM_WINDOWS
		u_long value = blocking ? 0 : 1;
		if (ioctlsocket(socket, FIONBIO, &value) == SOCKET_ERROR)
			return logger.Error(TC("Setting non blocking socket failed (error: %s)"), LastErrorToText(WSAGetLastError()).data);
#else
		int flags = fcntl(socket, F_GETFL, 0);
		if (flags == -1) return false;
		flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
		if (fcntl(socket, F_SETFL, flags) != 0)
			return logger.Error(TC("Setting non blocking socket failed (error: %s)"), LastErrorToText(WSAGetLastError()).data);
#endif
		return true;
	}

	bool DisableNagle(Logger& logger, SOCKET socket)
	{
#if !PLATFORM_MAC
		u32 value = 1;
		if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&value, sizeof(value)) == SOCKET_ERROR)
			return logger.Error(TC("setsockopt TCP_NODELAY error: (error: %s)"), LastErrorToText(WSAGetLastError()).data);
#endif
		return true;
	}

	bool SetTimeout(Logger& logger, SOCKET socket, u32 timeoutMs)
	{
		#if PLATFORM_WINDOWS
		DWORD timeout = timeoutMs;
		#else
		struct timeval timeout;
		timeout.tv_sec = timeoutMs / 1000;
		timeout.tv_usec = (timeoutMs % 1000)*1000;
		#endif

		if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof timeout) == SOCKET_ERROR)
			return logger.Error(TC("setsockopt SO_SNDTIMEO error: (error: %s)"), LastErrorToText(WSAGetLastError()).data);
		if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout) == SOCKET_ERROR)
			return logger.Error(TC("setsockopt SO_RCVTIMEO error: (error: %s)"), LastErrorToText(WSAGetLastError()).data);
		return true;
	}

	bool SetLinger(Logger& logger, SOCKET socket, u32 lingerSeconds)
	{
#if PLATFORM_MAC // Mac does not seem to automatically close the socket if the process crashes
		struct linger so_linger;
		so_linger.l_onoff = 1;
		so_linger.l_linger = u16(lingerSeconds);

		if (setsockopt(socket, SOL_SOCKET, SO_LINGER, (const char*)&so_linger, sizeof(so_linger)) < 0)
			return logger.Error(TC("setsockopt SO_LINGER error: (error: %s)"), LastErrorToText(WSAGetLastError()).data);
#endif
		return true;
	}

	bool SetRecvBuf(Logger& logger, SOCKET socket, u32 windowSize)
	{
		if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (const char*)&windowSize, sizeof(windowSize)) == SOCKET_ERROR)
			return logger.Error(TC("setsockopt TCP_NODELAY error: (error: %s)"), LastErrorToText(WSAGetLastError()).data);
		return true;
	}

	bool SetSendBuf(Logger& logger, SOCKET socket, u32 windowSize)
	{
		if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char*)&windowSize, sizeof(windowSize)) == SOCKET_ERROR)
			return logger.Error(TC("setsockopt TCP_NODELAY error: (error: %s)"), LastErrorToText(WSAGetLastError()).data);
		return true;
	}

	bool SetKeepAlive(Logger& logger, SOCKET socket) // This will make sure that WSAPoll exits when the network cable is pulled
	{
		
		int keepAliveTime = KeepAliveIdleSeconds; // Time of idle before it starts sending keep alive
		int keepAliveTimeInterval = KeepAliveIntervalSeconds; // Time between each check (10 failures mean disconnect)


		u32 value = 1;
		if (setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&value, sizeof(value)) == SOCKET_ERROR)
			return logger.Error(TC("setsockopt SO_KEEPALIVE (error: %s)"), LastErrorToText(WSAGetLastError()).data);

#if PLATFORM_WINDOWS	
		struct tcp_keepalive kaSettings;
		DWORD bytesReturned;
		kaSettings.onoff = 1;
		kaSettings.keepalivetime = keepAliveTime * 1000;
		kaSettings.keepaliveinterval = keepAliveTimeInterval * 1000; 
		if (WSAIoctl(socket, SIO_KEEPALIVE_VALS, &kaSettings, sizeof(kaSettings), NULL, 0, &bytesReturned, NULL, NULL) == SOCKET_ERROR)
			return logger.Error(TC("WSAIoctl SIO_KEEPALIVE_VALS (error: %s)"), LastErrorToText(WSAGetLastError()).data);
#elif PLATFORM_LINUX
		if (setsockopt(socket, IPPROTO_TCP, TCP_KEEPIDLE, &keepAliveTime, sizeof(int)) < 0)
			return logger.Error(TC("setsockopt TCP_KEEPIDLE (error: %s)"), LastErrorToText(WSAGetLastError()).data);
		if (setsockopt(socket, IPPROTO_TCP, TCP_KEEPINTVL, &keepAliveTimeInterval, sizeof(int)) < 0)
			return logger.Error(TC("setsockopt TCP_KEEPINTVL (error: %s)"), LastErrorToText(WSAGetLastError()).data);
		int keepAliveProbes = KeepAliveProbeCount; // Number of tests before timing out
		if (setsockopt(socket, IPPROTO_TCP, TCP_KEEPCNT, &keepAliveProbes, sizeof(int)) < 0)
			return logger.Error(TC("setsockopt TCP_KEEPCNT (error: %s)"), LastErrorToText(WSAGetLastError()).data);
#else // PLATFORM_MAC
		if (setsockopt(socket, IPPROTO_TCP, TCP_KEEPALIVE, &keepAliveTime, sizeof(int)) < 0)
			return logger.Error(TC("setsockopt TCP_KEEPALIVE (error: %s)"), LastErrorToText(WSAGetLastError()).data);
#endif
		return true;
	}

	bool NetworkBackendTcp::SendSocket(Connection& connection, Logger& logger, const void* b, u64 bufferLen, const tchar* hint)
	{
		#if UBA_EMULATE_BAD_INTERNET
		if ((rand() % 10000) == 0)
		{
			connection.logger.Info(TC("BAD INTERNET"));
			Sleep(10000);
		}
		#endif

		SOCKET socket = connection.socket;
		char* buffer = (char*)b;
		u64 left = bufferLen;
		while (left)
		{
			int sent = (int)send(socket, (char*)buffer, u32(left), 0);
			if (sent == SOCKET_ERROR)
			{
				u32 lastError = WSAGetLastError();
				if (lastError == WSAEWOULDBLOCK)
				{
					u64 timeoutMs = 40 * 1000; // 40 seconds timeout to begin with

					for (u32 i=0; i!=2; ++i)
					{
						WSAPOLLFD p;
						p.fd = socket;
						p.revents = 0;
						p.events = POLLWRNORM;
						int res = WSAPoll(&p, 1, int(timeoutMs));
						if (!res)
						{
							if (i == 0)
							{
								logger.Info(TC("SendSocket - WSAPoll returned timeout after 40 seconds for socket %i (%s)"), socket, hint);
								timeoutMs = DefaultNetworkSendTimeoutSeconds * 1000; // 10 minutes send timeout
								continue;
							}
							logger.Info(TC("SendSocket - WSAPoll returned timeout after 10 minutes for socket %i (%s)"), socket, hint);
							return false;
						}
						if (res == SOCKET_ERROR)
						{
							#if UBA_LOG_SOCKET_ERRORS
							logger.Info(TC("SendSocket - WSAPoll returned an error for socket %i: %s (%s)"), socket, LastErrorToText(WSAGetLastError()).data, hint);
							#endif
							return false;
						}
						break;
					}
					continue;
				}
				#if UBA_LOG_SOCKET_ERRORS
				logger.Info(TC("SendSocket - send returned an error for socket %i: %s (%s)"), socket, LastErrorToText(WSAGetLastError()).data, hint);
				#endif
				return false;
			}

			buffer += sent;
			left -= sent;
		}
		return true;
	}

	bool NetworkBackendTcp::RecvSocket(Connection& connection, RecvCache& recvCache, void* b, u32& bufferLen, const tchar* hint, bool isFirstCall, bool allowLess)
	{
		#if UBA_EMULATE_BAD_INTERNET
		if ((rand() % 10000) == 0)
		{
			connection.logger.Info(TC("BAD INTERNET"));
			Sleep(10000);
		}
		#endif

		SOCKET socket = connection.socket;

		u8* buffer = (u8*)b;
		u32 recvLeft = bufferLen;
		while (recvLeft)
		{
			if (recvCache.byteCount)
			{
				u32 toCopy = Min(recvCache.byteCount, recvLeft);
				memcpy(buffer, recvCache.bytes, toCopy);
				recvCache.byteCount -= toCopy;
				if (recvCache.byteCount)
					memmove(recvCache.bytes, recvCache.bytes + toCopy, recvCache.byteCount);
				recvLeft -= toCopy;
				buffer += toCopy;
				if (!recvLeft)
					return true;
			}

			u8* writePos;
			u32 writeCapacity;

			if (sizeof(recvCache.bytes) < recvLeft)
			{
				writePos = buffer;
				writeCapacity = recvLeft;
			}
			else
			{
				writePos = recvCache.bytes + recvCache.byteCount;
				writeCapacity = sizeof(recvCache.bytes) - recvCache.byteCount;
			}

			int read = (int)recv(socket, (char*)writePos, writeCapacity, 0);

			if (read > 0)
			{
				if (writePos == buffer)
				{
					recvLeft -= read;
					buffer += read;
				}
				else
				{
					recvCache.byteCount += read;
					UBA_ASSERT(recvCache.byteCount <= sizeof(recvCache.bytes));
					u32 toCopy = Min(recvCache.byteCount, recvLeft);
					memcpy(buffer, recvCache.bytes, toCopy);
					recvCache.byteCount -= toCopy;
					if (recvCache.byteCount)
						memmove(recvCache.bytes, recvCache.bytes + toCopy, recvCache.byteCount);
					recvLeft -= toCopy;
					buffer += toCopy;
				}

				if (recvLeft == 0)
					return true;
			
				if (allowLess)
				{
					bufferLen = read;
					return true;
				}
				continue;
			}

			if (read == 0)
			{
				#if UBA_LOG_SOCKET_ERRORS
				connection.logger.Info(TC("RecvSocket - recv gracefully closed by peer for socket %i and connection %s (%s%s)"), socket, GuidToString(connection.uid).str, connection.recvHint, hint);
				#endif
				return false;
			}

			UBA_ASSERT(read == SOCKET_ERROR);

			if (WSAGetLastError() == WSAEWOULDBLOCK)
			{
				u32 timeoutMs = connection.recvTimeoutMs;
				if (!timeoutMs)
					timeoutMs = DefaultNetworkReceiveTimeoutSeconds * 1000; // 10 minutes time out

				WSAPOLLFD p;
				p.fd = socket;
				p.revents = 0;
				p.events = POLLRDNORM;
				int res = WSAPoll(&p, 1, int(timeoutMs));
				if (!res)
				{
					if (connection.recvTimeoutCallback)
					{
						if (connection.recvTimeoutCallback(connection.recvTimeoutContext, timeoutMs, connection.recvHint, hint))
							continue;
						return false;
					}
					connection.logger.Info(TC("RecvSocket - WSAPoll returned timeout for socket %i and connection %s after %s (%s%s)"), socket, GuidToString(connection.uid).str, TimeToText(MsToTime(timeoutMs)).str, connection.recvHint, hint);
					return false;
				}
				if (res == SOCKET_ERROR)
				{
					#if UBA_LOG_SOCKET_ERRORS
					connection.logger.Info(TC("RecvSocket - WSAPoll returned an error for socket %i and connection %s: %s (%s%s)"), socket, GuidToString(connection.uid).str, LastErrorToText(WSAGetLastError()).data, connection.recvHint, hint);
					#endif
					return false;
				}
				continue;
			}

			#if !PLATFORM_WINDOWS
			if (!isFirstCall && errno != ECONNRESET)
				return connection.logger.Warning(TC("RecvSocket - recv error on socket %i and connection %s: %s (%s%s)"), socket, GuidToString(connection.uid).str, strerror(errno), connection.recvHint, hint);
			#endif

			#if UBA_LOG_SOCKET_ERRORS
			connection.logger.Info(TC("RecvSocket - read returned an error for socket %i and connection %s: %s (%s%s)"), socket, GuidToString(connection.uid).str, LastErrorToText(WSAGetLastError()).data, connection.recvHint, hint);
			#endif
			return false;
		}
		return true;
	}

	void TraverseNetworkAddresses(Logger& logger, const Function<bool(const StringBufferBase& addr)>& func)
	{
#if PLATFORM_WINDOWS
		// Fallback code for some cloud setups where we can't use the dns to find out ip addresses. (note it always work by providing the adapter we want to listen on)
		IP_ADAPTER_INFO info[16];
		ULONG bufLen = sizeof(info);
		if (GetAdaptersInfo(info, &bufLen) != ERROR_SUCCESS)
		{
			logger.Info(TC("GetAdaptersInfo failed (%s)"), LastErrorToText(WSAGetLastError()).data);
			return;
		}
		for (IP_ADAPTER_INFO* it = info; it; it = it->Next)
		{
			if (it->Type != MIB_IF_TYPE_ETHERNET && it->Type != IF_TYPE_IEEE80211)
				continue;
			for (IP_ADDR_STRING* s = &it->IpAddressList; s; s = s->Next)
			{
				StringBuffer<128> ip;
				ip.Appendf(TC("%hs"), s->IpAddress.String);
				if (ip.Equals(L"0.0.0.0"))
					continue;
				if (!func(ip))
					return;
			}
		}
#else
		struct ifaddrs* ifaddr;
		if (getifaddrs(&ifaddr) == -1)
		{
			logger.Info("getifaddrs failed");
			return;
		}
		auto g = MakeGuard([ifaddr]() { freeifaddrs(ifaddr); });

		for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next)
		{
			if (ifa->ifa_addr == nullptr)
				continue;

			int family = ifa->ifa_addr->sa_family;
			if (family != AF_INET)
				continue;

			StringBuffer<NI_MAXHOST> ip;
			int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), ip.data, ip.capacity, NULL, 0, NI_NUMERICHOST);
			if (s != 0)
				continue;
			ip.count = strlen(ip.data);
			if (ip.StartsWith("169.254") || ip.Equals("127.0.0.1"))
				continue;
			if (!func(ip))
				return;
		}
#endif
	}

	bool TraverseRemoteAddresses(Logger& logger, const tchar* addr, u16 port, const Function<bool(const sockaddr& remoteSockaddr)>& func)
	{
		addrinfoW  hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET; //AF_UNSPEC; (Skip AF_INET6)
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		StringBuffer<32> portStr;
		portStr.AppendValue(port);

		// Resolve the server address and port
		addrinfoW* remoteAddrInfo = nullptr;
		int res = GetAddrInfoW(addr, portStr.data, &hints, &remoteAddrInfo);
		if (res != 0)
		{
			if (res == WSAHOST_NOT_FOUND)
				return logger.Error(TC("Invalid server address '%s'"), addr);
			//logger.Error(TC("GetAddrInfoW failed with error: %s"), getErrorText(res).c_str());
			return false;
		}

		auto addrCleanup = MakeGuard([&]() { if (remoteAddrInfo) FreeAddrInfoW(remoteAddrInfo); });

		auto addrInfoIt = remoteAddrInfo;
		// Loop through and attempt to connect to an address until one succeeds
		for (; addrInfoIt != NULL; addrInfoIt = addrInfoIt->ai_next)
			if (!func(*addrInfoIt->ai_addr))
				return true;
		return true;
	}

	HttpConnection::HttpConnection()
	{
		m_socket = INVALID_SOCKET;
		*m_host = 0;
	}

	HttpConnection::~HttpConnection()
	{
		if (m_socket != INVALID_SOCKET)
		{
			LoggerWithWriter logger(g_nullLogWriter);
			CloseSocket(logger, m_socket, TC("HttpDtor"));
		}

		#if PLATFORM_WINDOWS
		if (m_wsaInitDone)
			WSACleanup();
		#endif
	}

	bool HttpConnection::Connect(Logger& logger, const char* host)
	{
		#if PLATFORM_WINDOWS
		WSADATA wsaData;
		if (!m_wsaInitDone)
			if (int res = WSAStartup(MAKEWORD(2, 2), &wsaData))
				return logger.Error(TC("WSAStartup failed (%d)"), res);
		m_wsaInitDone = true;
		#endif

		hostent* hostent = gethostbyname(host);
		if (hostent == NULL)
			return logger.Error(TC("HttpConnection: gethostbyname error (%s)"), host);

		char* ntoaRes = inet_ntoa(*(struct in_addr*)*(hostent->h_addr_list));
		if (!ntoaRes)
			return logger.Error(TC("HttpConnection: inet_ntoa returned null"));
		unsigned long in_addr = inet_addr(ntoaRes);
		if (in_addr == INADDR_NONE)
			return logger.Error(TC("HttpConnection: inet_addr returned INADDR_NONE (%s)"), ntoaRes);

		protoent* protoent = getprotobyname("tcp");
		if (protoent == NULL)
			return logger.Error(TC("HttpConnection: getprotobyname returned null for tcp"));

		SOCKET sock = socket(AF_INET, SOCK_STREAM, protoent->p_proto);
		if (sock == INVALID_SOCKET)
			return logger.Error(TC("HttpConnection: socket failed (%s)"), LastErrorToText(WSAGetLastError()).data);
		auto socketClose = MakeGuard([sock]() { closesocket(sock); });

		if (m_connectTimeOutMs)
			SetTimeout(logger, sock, m_connectTimeOutMs);

		sockaddr_in sockaddr_in;
		sockaddr_in.sin_addr.s_addr = in_addr;
		sockaddr_in.sin_family = AF_INET;
		sockaddr_in.sin_port = htons(80);

		if (connect(sock, (struct sockaddr*)&sockaddr_in, sizeof(sockaddr_in)) == -1)
			return false;// logger.Error(TC("HttpConnection: connect error"));

		socketClose.Cancel();

		if (m_connectTimeOutMs)
			SetTimeout(logger, sock, 10000); // 10 seconds timeout

		strcpy_s(m_host, sizeof_array(m_host), host);
		m_socket = sock;
		return true;
	}

	bool HttpConnection::Query(Logger& logger, const char* type, StringBufferBase& outResponse, u32& outStatusCode, const char* host, const char* path, const char* header, u32 timeoutMs)
	{
		// TODO: Fix so we reuse socket connection for multiple queries
		// Will need to change "Connection: close" and also know where end of message is

		if (*m_host)// && _stricmp(m_host, host) != 0)
		{
			CloseSocket(logger, m_socket, TC("HttpQuery"));
			m_socket = INVALID_SOCKET;
			*m_host = 0;
		}

		if (m_socket == INVALID_SOCKET)
			if (!Connect(logger, host))
				return false;

		char request[1024];
		int requestLen = snprintf(request, sizeof_array(request), "%s /%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: uba\r\nConnection: close\r\n%s\r\n", type, path, m_host, header);
		if (requestLen >= sizeof_array(request))
			return logger.Error(TC("STACK BUFFER TOO SMALL!"));

		SetTimeout(logger, m_socket, timeoutMs);

		int totalBytesSent = 0;
		while (totalBytesSent < requestLen) {
			int bytesSent = send(m_socket, request + totalBytesSent, requestLen - totalBytesSent, 0);
			if (bytesSent == -1)
				return logger.Error(TC("HttpConnection: socket send error (%hs)"), host);
			totalBytesSent += bytesSent;
		}

		//logger.Warning(TC("REQUEST:\r\n%hs\r\n"), request);

#if PLATFORM_WINDOWS
#pragma warning(push)
#pragma warning(disable:6386) // analyzer claims that buf can have buffer overrun.. but can't see how that can happen
#endif

		u32 readPos = 0;
		char buf[4*1024];
		int bytesRead = 0;
		while ((bytesRead = recv(m_socket, buf + readPos, sizeof(buf) - readPos, 0)) > 0)
			readPos += bytesRead;

		if (bytesRead == SOCKET_ERROR)
			return logger.Error(TC("HttpConnection: socket recv error after reading %u bytes - %s (%hs %hs)"), readPos, LastErrorToText(WSAGetLastError()).data, m_host, path);

		if (readPos == sizeof(buf))
			return logger.Error(TC("HttpConnection: buffer overflow"));

		buf[readPos] = 0;

#if PLATFORM_WINDOWS
#pragma warning(pop)
#endif

		//logger.Warning(TC("RESPONSE:\r\n%hs\r\n"), buf);

		char* firstSpace = strchr(buf, ' '); // After version (where status code starts)
		if (!firstSpace)
			return logger.Error(TC("HttpConnection: first space not found (read %u)"), readPos);

		char* secondSpace = strchr(firstSpace + 1, ' '); // after status code
		if (!secondSpace)
			return logger.Error(TC("HttpConnection: second space not found"));

		*secondSpace = 0;
		outStatusCode = strtoul(firstSpace + 1, nullptr, 10);

		if (outStatusCode != 200)
			return false;

		char* bodyStart = strstr(secondSpace + 1, "\r\n\r\n");
		if (!bodyStart)
			return logger.Error(TC("HttpConnection: no body found"));

		outResponse.Append(bodyStart + 4);
		return true;
	}

	void HttpConnection::SetConnectTimeout(u32 timeOutMs)
	{
		m_connectTimeOutMs = timeOutMs;
	}
}
