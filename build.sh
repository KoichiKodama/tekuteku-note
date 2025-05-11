#! /bin/sh
g++ -std=c++17 -O2 -DUSE_SSL -I. -I../boost_1_84_0 tekuteku_server.cpp -L../boost_1_84_0/stage/lib -lboost_coroutine -lboost_context -lboost_filesystem -lboost_system -lcrypto -lssl -lpthread -o tekuteku_server

