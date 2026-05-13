#ifndef TEST_HTTP_SERVER_H
#define TEST_HTTP_SERVER_H

// Brings up a SoftAP, starts an IdfHttpServer on port 80, and registers a
// handful of demo routes. Blocks forever so the server stays available
// for manual probing from a phone or laptop.
//
// Connect from a client:
//   1. Join Wi-Fi network "airgradient-ref" (password: "airgradient")
//   2. http://192.168.4.1/             -> embedded index.html
//   3. http://192.168.4.1/api/status   -> JSON status (GET)
//   4. http://192.168.4.1/api/echo     -> POST any body, get it back
void run_test_http_server();

#endif // TEST_HTTP_SERVER_H
