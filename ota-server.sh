#!/bin/sh

openssl s_server -WWW -key ./certs/deltahalo.key -cert ./certs/deltahalo.crt -port 8070 -state
