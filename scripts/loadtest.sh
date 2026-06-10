#!/bin/bash

# Navigate to the test_client directory relative to where the script is located
cd "$(dirname "$0")/../test_client" || exit 1

node index.js \
    --sip-server-ip 127.0.0.1 \
    --sip-server-port 5060 \
    --client-ip 127.0.0.1 \
    --client-rtp-port-base 25000 \
    --client-sip-port 5061 \
    --event-port 9000 \
    --calls 4000 \
    --call-rate 100 \
    --record one