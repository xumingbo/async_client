# async_client
Sample Code of Async TCP Client

## using libevent

<p/>the source file async_client.cc is self testable. It contains:
    <p/>class AsyncClient
    <p/>class MockupServer
    <p/>main() function to run the test
   
To build:
1. git clone https://github.com/libevent/libevent.git
2. build libevent 
3. cp async_client.cc libevent/sample
4. cp build_async_client.sh libevent/
5. cd libevent
6. ./build_async_client.sh

To run:
sample/async_client
