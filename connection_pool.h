/*
Author: Hongnan Zhang
Class: ECE6122
Last Date Modified: 2022/12/6
Description:
	Define the connection pool class using singleton pattern.
	The resources in the connection pool are a set of database connections that are statically allocated ,and dynamically used and released by the process.
	When the process needs to access the database, it is allocated a free connection from pool,
	and then the process releases the database connection after completing the database operation.
*/

#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>

#include "locker.h"

using namespace std;

class connection_pool
{
public:
	// Get Mysql connection
	MYSQL *GetConnection();
	// Release Mysql connection
	bool ReleaseConnection(MYSQL *conn);
	// Get a free connection from pool
	int GetFreeConn();
	// Destory all connctions
	void DestroyPool();

	// Using singleton pattern
	static connection_pool *GetInstance();

	// Initialization
	void init(string url, string User, string PassWord, string DataBaseName, int Port, unsigned int MaxConn); 
	
	connection_pool();
	~connection_pool();

private:
	// Max number of connections in the pool
	unsigned int MaxConn;
	// Connections currently put into use
	unsigned int CurConn;
	// Connection currently free
	unsigned int FreeConn;

private:
	locker lock;
	// List of connection pool
	list<MYSQL *> connList;
	sem reserve;

private:
	// Server ddress
	string url;
	string Port;
	// User name and password
	string User;
	string PassWord;
	// Database name
	string DatabaseName;
};

class connectionRAII{

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;
};

#endif
