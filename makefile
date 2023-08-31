server: main.cpp thread_pool.h http_handler.cpp http_handler.h locker.h log.cpp log.h connection_pool.cpp connection_pool.h
	g++ -o server main.cpp thread_pool.h http_handler.cpp http_handler.h locker.h log.cpp log.h connection_pool.cpp connection_pool.h -lpthread -lmysqlclient

clean:
	rm  -r server
