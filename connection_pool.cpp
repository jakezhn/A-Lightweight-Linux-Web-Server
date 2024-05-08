#include <mysql/mysql.h>
#include <iostream>
#include <string>
#include <list>
#include <mutex>
#include <semaphore.h> // C++20, use alternative for older standards

#include "connection_pool.h"

using namespace std;

// Singleton pattern implementation for connection pool
connection_pool* connection_pool::GetInstance() {
    static connection_pool instance;
    return &instance;
}

connection_pool::connection_pool() : CurConn(0), FreeConn(0) {}

void connection_pool::init(const string& url, const string& User, const string& PassWord, const string& DBName, int Port, unsigned int MaxConn) {
    this->url = url;
    this->Port = Port;
    this->User = User;
    this->PassWord = PassWord;
    this->DatabaseName = DBName;

    lock_guard<mutex> guard(lock); // Use RAII for locking
    for (unsigned int i = 0; i < MaxConn; i++) {
        MYSQL *con = mysql_init(nullptr);
        if (!con) {
            cerr << "Error: " << mysql_error(con) << endl;
            throw runtime_error("Failed to init MYSQL connection");
        }
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, nullptr, 0);
        if (!con) {
            cerr << "Error: " << mysql_error(con) << endl;
            throw runtime_error("Failed to connect to MYSQL server");
        }
        connList.push_back(con);
        FreeConn++;
    }
    reserve = semaphore(FreeConn);
    this->MaxConn = FreeConn;
}

MYSQL* connection_pool::GetConnection() {
    if (connList.empty()) {
        return nullptr;
    }

    reserve.acquire(); // Block until a connection is available
    lock_guard<mutex> guard(lock);
    MYSQL* con = connList.front();
    connList.pop_front();

    --FreeConn;
    ++CurConn;

    return con;
}

bool connection_pool::ReleaseConnection(MYSQL* con) {
    if (!con) {
        return false;
    }

    lock_guard<mutex> guard(lock);
    connList.push_back(con);
    ++FreeConn;
    --CurConn;

    reserve.release(); // Signal that a connection is available
    return true;
}

void connection_pool::DestroyPool() {
    lock_guard<mutex> guard(lock);
    for (auto con : connList) {
        mysql_close(con);
    }
    connList.clear();
    CurConn = 0;
    FreeConn = 0;
}

int connection_pool::GetFreeConn() {
    return FreeConn;
}

connection_pool::~connection_pool() {
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {
    *SQL = connPool->GetConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(conRAII);
}
