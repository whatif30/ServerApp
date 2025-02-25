#include "ServerApp.h"

#include "Packet.h"
#include "Client.h"
#include "Database.h"
#include "ReceiptVerifier.h"
#include "ResourceManager.h"

#include <stdio.h>
#include <vector>
#include <memory>

#pragma comment( lib, "Ws2_32.lib" )

////////////////////////////////////
// Socket structure
// Packet entry enum
////////////////////////////////////
struct SOCKETINFO {
	SOCKETINFO()
	{
		memset( this, 0, sizeof( SOCKETINFO ) );

		dataBuffer.len = Config::SOCKET_BUFFER_SIZE;
		dataBuffer.buf = msgBuffer;
	}

	DWORD GetPtr() { return reinterpret_cast< DWORD >( this ); }

	void ClearBuffer()
	{
		memset( msgBuffer, 0, Config::SOCKET_BUFFER_SIZE );
		dataBuffer.len = Config::SOCKET_BUFFER_SIZE;
		dataBuffer.buf = msgBuffer;
	}

	WSAOVERLAPPED	overlapped;
	WSABUF			dataBuffer;
	SOCKET			socket;
	char			msgBuffer[ Config::SOCKET_BUFFER_SIZE ];
};

ServerApp::ServerApp() :
	m_sockListener( NULL )
{
	OnCreate();
}

ServerApp::~ServerApp()
{
	OnDestroy();
}

void ServerApp::OnCreate()
{
	//Database::GetInstance();
	//ReceiptVerifier::GetInstance();
}

void ServerApp::OnDestroy()
{
	CloseSocket();
}

void ServerApp::Run()
{
	ResourceManager::Initiate();

	InitSocket();

	std::thread tListener( &ServerApp::AcceptThread, this );
	/*ReceiptVerifier::GetInstance()->VerifyReceipt( L"com.Cobalt.InAppPurchaseTest",
												   ReceiptVerifier::PurchaseType::PRODUCT,
												   L"com.Cobalt.InAppPurchaseTest.global8719",
												   L"b46ec4a6-b14a-42dc-8dee-5ae3b0e4966b" );*/
	while ( 1 ) {
		Sleep( 1 );

		clock_t tCur = clock();

		UpdateClient();
		//ReceiptVerifier::GetInstance()->Update( tCur );
	}
}

void ServerApp::InitSocket()
{
	// 0. Startup
	WSADATA wsaData;
	WORD wVersion = MAKEWORD( 2, 2 );
	int nError = WSAStartup( wVersion, &wsaData );
	if ( nError ) {
		printf( "Error - Failed to initiate socket\n" );
		return;
	}
	
	// 1. Create listener socket
	m_sockListener = WSASocketW( AF_INET,
									SOCK_STREAM,
									IPPROTO_TCP,
									NULL,
									NULL,
									WSA_FLAG_OVERLAPPED );
	if ( m_sockListener == INVALID_SOCKET ) {
		printf( "Error - Failed to create listner socket\n" );
		return;
	}

	// 2. Set server address
	SOCKADDR_IN serverAddr;
	memset( &serverAddr, 0, sizeof( SOCKADDR_IN ) );
	serverAddr.sin_family = PF_INET;
	serverAddr.sin_port = htons( Config::SERVER_PORT );
	serverAddr.sin_addr.S_un.S_addr = htonl( INADDR_ANY );

	// 3. Bind listener socket
	nError = bind( m_sockListener, reinterpret_cast< sockaddr* >( &serverAddr ), sizeof( SOCKADDR_IN ) );
	if ( nError == SOCKET_ERROR )
	{
		printf( "Error - Failed to bind listner socket\n" );

		closesocket( m_sockListener );

		WSACleanup();
		return;
	}

	// 4. Start listening
	nError = listen( m_sockListener, 5 );
	if ( nError == SOCKET_ERROR )
	{
		printf( "Error - Failed to listen\n" );
		
		closesocket( m_sockListener );
		
		WSACleanup();
		return;
	}

	// 5. Initiate IOCP and worker threads
	m_hIOCP = CreateIoCompletionPort( INVALID_HANDLE_VALUE, NULL, 0, 0 );

	SYSTEM_INFO systemInfo;
	GetSystemInfo( &systemInfo );
	int nThreadCount = systemInfo.dwNumberOfProcessors * 2;
	DWORD dwTID;

	for ( int iter = 0; iter < nThreadCount; ++iter ) {
		HANDLE hThread = CreateThread( NULL,
									   0,
									   WorkerThread,
									   &m_hIOCP,
									   NULL,
									   &dwTID );
		printf( "Created worker thread [%d]\n", hThread );
	}
}

void ServerApp::UpdateClient()
{
	std::lock_guard<std::mutex> lock( m_lockMapClient );

	for ( auto& pairClient : m_mpClient ) {
		std::shared_ptr<Client> pClient = pairClient.second;
		pClient->Update();
	}
}

void ServerApp::CloseSocket()
{
	for ( auto pClient : m_mpClient ) {
		pClient.second->Disconnect();
	}

	closesocket( m_sockListener );

	WSACleanup();
}

DWORD WINAPI ServerApp::WorkerThread( LPVOID hIOCP )
{
	HANDLE hThread = *( static_cast< HANDLE* >( hIOCP ) );
	unsigned __int64 completionKey;
	SOCKETINFO *eventSocket;
	DWORD nRecvBytes;

	while ( 1 )
	{
		int nResult = GetQueuedCompletionStatus( hThread, &nRecvBytes, &completionKey, reinterpret_cast< LPOVERLAPPED* >( &eventSocket ), INFINITE );

		if ( !nResult || nRecvBytes == 0 )
		{
			printf( "Client ID : %d Disconnected\n", eventSocket->socket );
			ServerApp::GetInstance()->DestroyClientConnection( eventSocket->socket );
			closesocket( eventSocket->socket );
			free( eventSocket );
		}
		else
		{
			InPacket iPacket( eventSocket->dataBuffer.buf );
			OnReceivePacket( eventSocket->socket, iPacket );

			eventSocket->ClearBuffer();

			DWORD flags = NULL;
			nResult = WSARecv( eventSocket->socket, &( eventSocket->dataBuffer ), 1, &nRecvBytes, &flags, &eventSocket->overlapped, NULL );
			if ( nResult == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING )
				printf( "Error - Failed WSARecv(error_code : %d)\n", WSAGetLastError() );
		}
	}
}

void ServerApp::AcceptThread()
{
	// 0. Client address initialization
	SOCKADDR_IN clientAddr;
	int nAddrLength = sizeof( SOCKADDR_IN );
	memset( &clientAddr, 0, nAddrLength );

	while ( 1 ) {
		SOCKET sockClient;
		DWORD dwRecvByteCount;

		// 1. Accept client and create TCP/IP socket
		sockClient = accept( m_sockListener, reinterpret_cast< sockaddr* >( &clientAddr ), &nAddrLength );
		if ( sockClient == INVALID_SOCKET )
		{
			printf( "Error - Accept Failure\n" );
			return;
		}

		// 2. Create IOCP and wait for client packet
		SOCKETINFO* si = new SOCKETINFO();
		si->socket = sockClient;

		m_hIOCP = CreateIoCompletionPort( reinterpret_cast< HANDLE >( sockClient ), m_hIOCP, si->GetPtr(), 0 );

		DWORD dwFlag = 0;
		BOOL bFailed = WSARecv( si->socket, &si->dataBuffer, 1, &dwRecvByteCount, &dwFlag, &( si->overlapped ), NULL );
		if ( bFailed && WSAGetLastError() != WSA_IO_PENDING )
		{
			printf( "Error - IO pending Failure\n" );
			return;
		}

		// 3. Create client instance
		CreateClientConnection( sockClient );
	}
}

void ServerApp::OnReceivePacket( const SOCKET& socketID, const InPacket& iPacket )
{
	auto pClient = ServerApp::GetInstance()->GetClient( socketID );
	pClient->OnPacket( iPacket );
}

void ServerApp::SendPacket( const SOCKET& socketID, OutPacket& oPacket )
{
	DWORD dwSendBytes = 0;

	_WSABUF buf;
	buf.buf = oPacket.GetBuffer();
	buf.len = oPacket.GetBufferSize();

	int nResult = WSASend( socketID, &buf, 1, &dwSendBytes, 0, NULL, NULL );

	if ( nResult == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING )
		printf( "Error - Failed WSASend(error_code : %d)\n", WSAGetLastError() );
}

void ServerApp::CreateClientConnection( const SOCKET& socketID )
{
	std::lock_guard<std::mutex> lock( m_lockMapClient );
	std::shared_ptr<Client> pClient( new Client( socketID ) );
	m_mpClient.emplace( std::make_pair( socketID, pClient ) );
	printf( "Client ID : %d Connected\n", socketID );
}

void ServerApp::DestroyClientConnection( const SOCKET& socketID )
{
	std::lock_guard<std::mutex> lock( m_lockMapClient );

	if ( m_mpClient.find( socketID ) == m_mpClient.end() ) return;

	m_mpClient.erase( socketID );
}

std::shared_ptr<Client> ServerApp::GetClient( const SOCKET& socketID )
{
	std::lock_guard<std::mutex> lock( m_lockMapClient );

	if ( m_mpClient.find( socketID ) == m_mpClient.end() ) return nullptr;

	return m_mpClient[ socketID ];
}
