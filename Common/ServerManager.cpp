#include "StdafxCommon.h"
#include "ServerManager.h"

enum SOCKET_INIT_SEQUENCE {
	SET_SERVER_ADDRESS,
	BIND_SOCKET,
	START_LISTEN
};

ServerManager::ServerManager() :
	m_sockListener( NULL ),
	m_bReady( false )
{
	OnCreate();
}

ServerManager::~ServerManager()
{
	OnDestroy();
}

void ServerManager::OnCreate()
{

}

void ServerManager::OnDestroy()
{
	if ( m_tListener.joinable() )
		m_tListener.join();

	if ( m_sockListener )
		closesocket( m_sockListener );
}

void ServerManager::Initiate( const int nWorkerThreadCount, const int nConnectionCount )
{
	// 0. Initiate IOCP and worker threads
	m_workerParam.hThread = CreateIoCompletionPort( INVALID_HANDLE_VALUE, NULL, 0, 0 );
	m_workerParam.pServerManager = this;

	DWORD dwTID;
	for ( int iter = 0; iter < nWorkerThreadCount; ++iter ) {
		HANDLE hThread = CreateThread( NULL,
									   0,
									   WorkerThread,
									   &m_workerParam,
									   NULL,
									   &dwTID );
		printf( "Created worker thread [%d]\n", hThread );
	}

	// 1. Allocate socket info
	m_lockSockInfo.lock();
	m_aSockInfo.resize( nConnectionCount );
	for ( int nIdx = 0; nIdx < nConnectionCount; ++nIdx ) {
		m_qSockIdxReady.push( nIdx );
		m_aSockInfo.at( nIdx ).nIdx = nIdx;
	}
	m_lockSockInfo.unlock();
}

void ServerManager::OpenSocket( const int nPort, const char* sTargetIP/*= nullptr*/ )
{
	// 0. Create listener socket
	m_sockListener = WSASocketW( AF_INET,
								 SOCK_STREAM,
								 IPPROTO_TCP,
								 NULL,
								 NULL,
								 WSA_FLAG_OVERLAPPED );
	if ( m_sockListener == INVALID_SOCKET ) {
		Debug::Log( "Error - Failed to create listener socket" );
		return;
	}

	// 1. Set server address
	SOCKADDR_IN serverAddr;
	memset( &serverAddr, 0, sizeof( SOCKADDR_IN ) );
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons( nPort );
	int nResult = 0;
	if ( sTargetIP ) {
		nResult = inet_pton( AF_INET, sTargetIP, &serverAddr.sin_addr.S_un.S_addr );
	}
	else
		serverAddr.sin_addr.S_un.S_addr = htonl( INADDR_ANY );
	if ( !HandleSocketError( nResult, SOCKET_INIT_SEQUENCE::SET_SERVER_ADDRESS, m_sockListener ) )
		return;

	// 2. Bind listener socket
	nResult = bind( m_sockListener, reinterpret_cast< sockaddr* >( &serverAddr ), sizeof( SOCKADDR_IN ) );
	if ( !HandleSocketError( nResult, SOCKET_INIT_SEQUENCE::BIND_SOCKET, m_sockListener ) )
		return;

	// 3. Start listening
	nResult = listen( m_sockListener, 5 ); // listen queue(5)
	if ( !HandleSocketError( nResult, SOCKET_INIT_SEQUENCE::START_LISTEN, m_sockListener ) )
		return;

	m_bReady = true;
	printf( "Listener socket [%u] ready!\n", m_sockListener );
}

void ServerManager::Run()
{
	if ( !m_bReady ) {
		Debug::Log( "Not ready" );
		return;
	}

	m_tListener = std::thread( &ServerManager::AcceptThread, this );
	m_tUpdate = std::thread( &ServerManager::UpdateThread, this );
}

void ServerManager::UpdateThread()
{
	while ( true ) {
		for ( auto& elem : m_mClient ) {
			elem.second->Update();
		}

		Sleep( 0 );
	}
}

void ServerManager::AcceptThread()
{
	// 0. Client address initialization
	SOCKADDR_IN clientAddr;
	int nAddrLength = sizeof( SOCKADDR_IN );
	memset( &clientAddr, 0, nAddrLength );

	while ( true ) {
		DWORD dwRecvByteCount;

		// 1. Accept client and create TCP/IP socket
		SOCKETINFO& si = PopSocketReady();
		si.socket = accept( m_sockListener, reinterpret_cast< sockaddr* >( &clientAddr ), &nAddrLength );
		if ( si.socket == INVALID_SOCKET )
		{
			printf( "Error - Accept Failure [%d]\n", WSAGetLastError() );
			return;
		}
		
		OnSocketAccept( si.socket );
		printf( "Socket [%u] accepted\n", si.socket );

		// 2. Create IOCP and wait for client packet

		m_workerParam.hThread = CreateIoCompletionPort( reinterpret_cast< HANDLE >( si.socket ),
														m_workerParam.hThread,
														si.socket,
														0 );

		DWORD dwFlag = 0;
		BOOL bFailed = WSARecv( si.socket,
								&si.dataBuffer,
								1,
								&dwRecvByteCount,
								&dwFlag,
								&( si.overlapped ),
								NULL );
		if ( bFailed && WSAGetLastError() != WSA_IO_PENDING )
		{
			Debug::Log( "Error - IO pending Failure" );
			return;
		}
	}
}

DWORD WINAPI ServerManager::WorkerThread( LPVOID pParam )
{
	WORKERPARAM* pWorkerParam = static_cast< WORKERPARAM* >( pParam );
	ServerManager* pServerManager = pWorkerParam->pServerManager;
	SOCKET completionKey;
	SOCKETINFO* clientSocket;
	DWORD nRecvBytes;

	while ( true )
	{
		int nResult = GetQueuedCompletionStatus( pWorkerParam->hThread,
												 &nRecvBytes, 
												 &completionKey, 
												 reinterpret_cast< LPOVERLAPPED* >( &clientSocket ), 
												 INFINITE );

		if ( !nResult || nRecvBytes == 0 )
		{
			printf( "Client ID : %u Disconnected\n", clientSocket->socket );
			closesocket( clientSocket->socket );
			pServerManager->PushSocketReady( clientSocket->nIdx );
			pServerManager->OnSocketClose( clientSocket->socket );
		}
		else
		{
			InPacket iPacket( clientSocket->dataBuffer.buf );
			pServerManager->OnPacket( clientSocket->socket, iPacket );

			clientSocket->ClearBuffer();

			DWORD flags = NULL;
			nResult = WSARecv( clientSocket->socket,
							   &clientSocket->dataBuffer,
							   1,
							   &nRecvBytes,
							   &flags,
							   &clientSocket->overlapped,
							   NULL );
			if ( nResult == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING )
				printf( "Error - Failed WSARecv(error_code : %d)\n", WSAGetLastError() );
		}
	}
}

void ServerManager::OnPacket( const SOCKET& socket, const InPacket& iPacket )
{
	std::unique_lock<std::mutex> lock( m_lockClient );

	if ( m_mClient.find( socket ) == m_mClient.end() )
		return;

	m_mClient.at( socket )->OnPacket( iPacket );
}

void ServerManager::OnSocketAccept( const SOCKET& socket )
{
	// Create client object
	std::unique_lock<std::mutex> lock( m_lockClient );
	
	auto deleter = []( ServerConnection* c ) { g_mp.Free<ServerConnection>( c ); };

	std::shared_ptr<ServerConnection> pClient( g_mp.Alloc<ServerConnection>(), deleter );
	m_mClient.emplace( socket, pClient );
}

void ServerManager::OnSocketClose( const SOCKET& socket )
{
	// Destroy client object
	std::unique_lock<std::mutex> lock( m_lockClient );
	
	if ( m_mClient.find( socket ) == m_mClient.end() )
		return;
	
	m_mClient.at( socket )->Destroy();
	m_mClient.erase( socket );
}

void ServerManager::SendPacket( const SOCKET& socket, OutPacket& oPacket )
{
	DWORD dwSendBytes = 0;

	_WSABUF buf;
	buf.buf = oPacket.GetBuffer();
	buf.len = oPacket.GetBufferSize();

	int nResult = WSASend( socket, &buf, 1, &dwSendBytes, 0, NULL, NULL );

	if ( nResult == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING )
		printf( "Error - Failed WSASend(error_code : %d)\n", WSAGetLastError() );
}

bool ServerManager::HandleSocketError( const int nResult, enum SOCKET_INIT_SEQUENCE eSequence, const SOCKET socket )
{
	if ( nResult != SOCKET_ERROR )
		return true;

	std::string sError;
	switch ( eSequence ) {
		case SOCKET_INIT_SEQUENCE::SET_SERVER_ADDRESS:	sError = "Error - Failed to set server address";	break;
		case SOCKET_INIT_SEQUENCE::BIND_SOCKET:			sError = "Error - Failed to bind listener socket";	break;
		case SOCKET_INIT_SEQUENCE::START_LISTEN:		sError = "Error - Failed to listen";				break;
		default:
			return false;
	}

	Debug::Log( sError );

	closesocket( socket );

	return false;
}

SOCKETINFO& ServerManager::PopSocketReady()
{
	std::unique_lock<std::mutex> lock( m_lockSockInfo );
	
	int nIdx = m_qSockIdxReady.front();
	m_qSockIdxReady.pop();
	m_sSockIdxAssigned.emplace( nIdx );

	return m_aSockInfo.at( nIdx );
}

void ServerManager::PushSocketReady( const int nIdx )
{
	std::unique_lock<std::mutex> lock( m_lockSockInfo );
	
	m_sSockIdxAssigned.erase( nIdx );
	m_qSockIdxReady.push( nIdx );
}
