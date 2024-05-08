#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <iostream>
#include <list>
#include <string>
#include <mysql/mysql.h>

#include "locker.h"

using namespace std;

class connection_pool {
public:
    connection_pool();
    ~connection_pool();

    // Singleton access
    static connection_pool* GetInstance();

    MYSQL* GetConnection();                        // Retrieve a connection from the pool
    bool ReleaseConnection(MYSQL* conn);           // Return a connection to the pool
    int GetFreeConn();                             // Get the number of available connections
    void DestroyPool();                            // Destroy all connections and clean up the pool

    void init(const string& url, const string& user, const string& password, const string& databaseName, int port, unsigned int maxConn);

private:
    unsigned int MaxConn;      // Maximum number of connections in the pool
    unsigned int CurConn;      // Number of connections currently in use
    unsigned int FreeConn;     // Number of connections currently available

    locker lock;               // Mutex for thread safety
    list<MYSQL*> connList;     // List of available connections
    sem reserve;               // Semaphore tracking available connections

    string url;                // Database URL
    int port;                  // Database port
    string user;               // Database user
    string password;           // Database password
    string databaseName;       // Database name
};

// RAII wrapper class for MySQL connections
class connectionRAII {
public:
    connectionRAII(MYSQL** con, connection_pool* connPool);
    ~connectionRAII();

private:
    MYSQL* conRAII;            // Pointer to the MySQL connection
    connection_pool* poolRAII; // Pointer to the connection pool
};

#endif // CONNECTION_POOL_H
