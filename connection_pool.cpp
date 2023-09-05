#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>

#include "connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	this->CurConn = 0;
	this->FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

// Initialization for connection pool
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn)
{
	// Initialize databese info and its content
	this->url = url;
	this->Port = Port;
	this->User = User;
	this->PassWord = PassWord;
	this->DatabaseName = DBName;

	// Create MaxConn connections in the pool
	lock.lock();
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			cout << "Error:" << mysql_error(con);
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			cout << "Error: " << mysql_error(con);
			exit(1);
		}
		// Update connection list and free connection counter
		connList.push_back(con);
		++FreeConn;
	}
	// Initialize connection pool semaphore to free connection counter
	reserve = sem(FreeConn);

	this->MaxConn = FreeConn;
	
	lock.unlock();
}


// When request arrives, acquire a free connection from pool, then update counters for free connections and connections currently in use
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;
	// Wait for a free connection (wait till semaphore > 1)
	reserve.wait();
	
	lock.lock();
	// Get first available connection
	con = connList.front();
	connList.pop_front();

	--FreeConn;
	++CurConn;

	lock.unlock();
	return con;
}

// Release current connection
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++FreeConn;
	--CurConn;

	lock.unlock();
	// Semaphore++
	reserve.post();
	return true;
}

// Destory pool
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		// Close all connections
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		CurConn = 0;
		FreeConn = 0;
		// Clear list
		connList.clear();

		lock.unlock();
	}

	lock.unlock();
}

// Get number of free connections
int connection_pool::GetFreeConn()
{
	return this->FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}