/*
Orbit: High-performance, zero-allocation bi-directional audio bridge.
Copyright (C) 2026 Mark Horila

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
const crypto = require('crypto');
const sip = require('sip');
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');

// Mock crypto.randomBytes so the 'ws' library generates the static key expected by the C server
const originalRandomBytes = crypto.randomBytes;
crypto.randomBytes = function(size) {
    if (size === 16) {
        return Buffer.from("the sample nonce", "utf8");
    }
    return originalRandomBytes.apply(this, arguments);
};

// Require 'ws' AFTER the mock so it captures the mocked version
const WebSocket = require('ws');

// --- Command Line Parsing ---
const args = process.argv.slice(2);
const options = {
    sipServerIp: '127.0.0.1',
    sipServerPort: 5060,
    clientIp: '127.0.0.1',
    clientRtpPortBase: 15000,
    clientSipPort: 5061,
    eventPort: 9000,
    inputFile: '../input_l16be_16000_mono.raw',
    calls: 1,
    callRate: 10, // Calls per second
    record: 'none', // 'all', 'one', 'none'
    recordDir: './recordings',
    listenOnly: false
};

for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === '--sip-server-ip') options.sipServerIp = args[++i];
    else if (arg === '--sip-server-port') options.sipServerPort = parseInt(args[++i]);
    else if (arg === '--client-ip') options.clientIp = args[++i];
    else if (arg === '--client-rtp-port-base') options.clientRtpPortBase = parseInt(args[++i]);
    else if (arg === '--client-sip-port') options.clientSipPort = parseInt(args[++i]);
    else if (arg === '--event-port') options.eventPort = parseInt(args[++i]);
    else if (arg === '--input-file') options.inputFile = args[++i];
    else if (arg === '--calls') options.calls = parseInt(args[++i]);
    else if (arg === '--call-rate') options.callRate = parseInt(args[++i]);
    else if (arg === '--record') options.record = args[++i];
    else if (arg === '--record-dir') options.recordDir = args[++i];
    else if (arg === '--listen-only') options.listenOnly = true;
    else if (arg === '--help') {
        console.log(`
Usage: node index.js [options]

Options:
  --sip-server-ip <ip>          SIP server IP (default: 127.0.0.1)
  --sip-server-port <port>      SIP server port (default: 5060)
  --client-ip <ip>              Client IP (default: 127.0.0.1)
  --client-rtp-port-base <port> Base port for RTP (default: 15000)
  --client-sip-port <port>      Client SIP listen port (default: 5061)
  --event-port <port>           UDP port to listen for bridge events (default: 9000)
  --input-file <path>           Input raw audio file (default: ../input_l16be_16000_mono.raw)
  --calls <number>              Number of concurrent calls to simulate (default: 1)
  --call-rate <number>          Calls to initiate per second (default: 10)
  --record <all|one|none>       Recording option (default: none)
  --record-dir <path>           Directory to save recordings (default: ./recordings)
  --listen-only                 Do not initiate calls, just listen for events and echo WS audio
        `);
        process.exit(0);
    }
}

// Ensure record directory exists if needed
if (options.record !== 'none') {
    if (!fs.existsSync(options.recordDir)) {
        fs.mkdirSync(options.recordDir, { recursive: true });
    }
}

// Pre-load input audio
let inputBuf = null;
try {
    inputBuf = fs.readFileSync(options.inputFile);
    console.log(`[INFO] Loaded ${options.inputFile} (${inputBuf.length} bytes)`);
} catch (e) {
    console.log(`[WARN] ${options.inputFile} not found. Streaming dummy noise.`);
}

// Active calls map: Call-ID -> Call instance
const activeCalls = new Map();

// Single Event Listener
const eventServer = dgram.createSocket('udp4');
eventServer.on('message', (msg) => {
    try {
        const msgStr = msg.toString();
        console.log(`[EVENT] Received event: ${msgStr}`);
        const payload = JSON.parse(msgStr);
        if (payload.event === 'call_answered' && payload.ws_url && payload.call_id) {
            let call = activeCalls.get(payload.call_id);
            if (!call && options.listenOnly) {
                call = new Call(activeCalls.size);
                call.callId = payload.call_id;
                activeCalls.set(call.callId, call);
                console.log(`[INFO] Created listener call for Call-ID: ${payload.call_id}`);
            }
            if (call) {
                call.onCallAnswered(payload.ws_url);
            } else {
                console.warn(`[EVENT] Received call_answered for unknown Call-ID: ${payload.call_id}`);
            }
        }
    } catch (e) {
        console.error('[EVENT] Failed to parse event JSON:', e);
    }
});
eventServer.bind(options.eventPort, '0.0.0.0', () => {
    console.log(`[EVENT] Listening for bridge events on UDP port ${options.eventPort}`);
});

// Single SIP Stack Listener
sip.start({ port: options.clientSipPort }, (rq) => {
    // We ignore unexpected incoming SIP requests for this test client
});

// Global RTP Ticker
setInterval(() => {
    for (const call of activeCalls.values()) {
        call.tickRtp();
    }
}, 20);

class Call {
    constructor(index) {
        this.index = index;
        this.callId = crypto.randomUUID();
        this.rtpPort = options.clientRtpPortBase + (index * 2);
        this.serverRtpPort = 0;
        this.ws = null;
        this.wsConnected = false;
        this.rtpSocket = null;
        this.rtpOut = null;
        this.inputOffset = 0;
        this.inviteReq = null;
        this.lastToHeader = null;
        this.ending = false;

        this.seq = 0;
        this.ts = 0;
        this.ssrc = 0x12345678 + index;
        this.rtpPacket = Buffer.alloc(12 + 640);
        this.rtpPacket[0] = 0x80;
        this.rtpPacket[1] = 96; // L16 @ 16000Hz

        const shouldRecord = options.record === 'all' || (options.record === 'one' && index === 0);
        if (shouldRecord) {
            const recordPath = path.join(options.recordDir, `output_rtp_${this.index}.raw`);
            this.rtpOut = fs.createWriteStream(recordPath);
        }
    }

    start() {
        activeCalls.set(this.callId, this);

        // Setup RTP Socket
        this.rtpSocket = dgram.createSocket('udp4');
        this.rtpSocket.on('message', (msg, rinfo) => {
            if (msg.length > 12 && this.rtpOut) {
                this.rtpOut.write(msg.slice(12));
            }
        });

        this.rtpSocket.bind(this.rtpPort, '0.0.0.0', () => {
            this.sendInvite();
        });
    }

    sendInvite() {
        const sdp = [
            `v=0`,
            `o=- 123456 123457 IN IP4 ${options.clientIp}`,
            `s=-`,
            `c=IN IP4 ${options.clientIp}`,
            `t=0 0`,
            `m=audio ${this.rtpPort} RTP/AVP 11`,
            `a=rtpmap:11 L16/16000`,
            `a=ptime:20`,
            `a=sendrecv`
        ].join('\r\n') + '\r\n';

        this.inviteReq = {
            method: 'INVITE',
            uri: `sip:bridge@${options.sipServerIp}:${options.sipServerPort}`,
            headers: {
                to: { uri: `sip:bridge@${options.sipServerIp}:${options.sipServerPort}` },
                from: { uri: `sip:client@${options.clientIp}`, params: { tag: crypto.randomBytes(4).toString('hex') } },
                'call-id': this.callId,
                cseq: { method: 'INVITE', seq: 1 },
                contact: [{ uri: `sip:client@${options.clientIp}` }],
                'content-type': 'application/sdp',
            },
            content: sdp
        };

        sip.send(this.inviteReq, (res) => {
            if (res.status === 200) {
                this.lastToHeader = res.headers.to;
                const serverSdp = res.content;
                const match = serverSdp.match(/m=audio (\\d+)/);
                this.serverRtpPort = match ? parseInt(match[1]) : 16000;

                // ACK the 200 OK
                const ackReq = {
                    method: 'ACK',
                    uri: this.inviteReq.uri,
                    headers: {
                        to: res.headers.to,
                        from: res.headers.from,
                        'call-id': this.callId,
                        cseq: { method: 'ACK', seq: 1 }
                    }
                };
                sip.send(ackReq);
            } else if (res.status >= 400) {
                console.error(`[Call ${this.index}] Call failed with status ${res.status}`);
                this.cleanup();
            }
        });
    }

    onCallAnswered(wsUrl) {
        this.ws = new WebSocket(wsUrl);
        this.ws.on('open', () => {
            this.wsConnected = true;
        });

        this.ws.on('message', (data, isBinary) => {
            if (isBinary) {
                this.ws.send(data);
            }
        });

        this.ws.on('close', () => {
            this.cleanup();
        });

        this.ws.on('error', (err) => {
            console.error(`[Call ${this.index}] WS Error:`, err.message);
            this.endCall();
        });
    }

    tickRtp() {
        if (!this.serverRtpPort || !this.wsConnected || this.ending) return;

        if (inputBuf && this.inputOffset >= inputBuf.length) {
            this.ending = true;
            this.endCall();
            return;
        }

        let chunkSize = 640;
        if (inputBuf && inputBuf.length - this.inputOffset < chunkSize) {
            chunkSize = inputBuf.length - this.inputOffset;
        }

        if (chunkSize === 0) return;

        this.rtpPacket.writeUInt16BE(this.seq++, 2);
        this.rtpPacket.writeUInt32BE(this.ts, 4);
        this.rtpPacket.writeUInt32BE(this.ssrc, 8);

        if (inputBuf && inputBuf.length > 0) {
            inputBuf.copy(this.rtpPacket, 12, this.inputOffset, this.inputOffset + chunkSize);
            this.inputOffset += chunkSize;
        } else {
            this.rtpPacket.fill(0, 12, 12 + chunkSize);
        }

        this.ts += chunkSize;

        this.rtpSocket.send(this.rtpPacket, 0, 12 + chunkSize, this.serverRtpPort, options.sipServerIp);
    }

    endCall() {
        if (this.inviteReq) {
            const byeReq = {
                method: 'BYE',
                uri: `sip:bridge@${options.sipServerIp}:${options.sipServerPort}`,
                headers: {
                    to: this.lastToHeader || { uri: `sip:bridge@${options.sipServerIp}:${options.sipServerPort}` },
                    from: this.inviteReq.headers.from,
                    'call-id': this.callId,
                    cseq: { method: 'BYE', seq: 2 }
                }
            };
            sip.send(byeReq, (res) => {
                // Ignore BYE response
            });
        }

        if (this.ws) {
            this.ws.close();
        } else {
            this.cleanup();
        }
    }

    cleanup() {
        if (this.rtpSocket) {
            this.rtpSocket.close();
            this.rtpSocket = null;
        }
        if (this.rtpOut) {
            this.rtpOut.end();
            this.rtpOut = null;
        }
        activeCalls.delete(this.callId);

        if (!options.listenOnly && activeCalls.size === 0 && callsStarted >= options.calls && !process.shuttingDown) {
            console.log("All calls finished. Exiting.");
            process.exit(0);
        }
    }
}

process.on('SIGINT', () => {
    console.log('\n[INFO] Caught interrupt signal. Sending BYE to all active calls...');
    process.shuttingDown = true;
    for (const call of activeCalls.values()) {
        call.endCall();
    }
    // Give SIP BYE requests a moment to hit the network before fully exiting
    setTimeout(() => process.exit(0), 500);
});

let callsStarted = 0;
const startInterval = 1000 / options.callRate;

function spawnNextCall() {
    if (callsStarted >= options.calls) return;

    const call = new Call(callsStarted);
    call.start();
    callsStarted++;

    if (callsStarted % 100 === 0 || callsStarted === options.calls) {
        console.log(`[INFO] Spawned ${callsStarted} / ${options.calls} calls`);
    }

    if (callsStarted < options.calls) {
        setTimeout(spawnNextCall, startInterval);
    }
}

// Start spawning calls
if (options.listenOnly) {
    console.log(`[INFO] Starting in listen-only mode. Waiting for events on UDP port ${options.eventPort}`);
} else {
    console.log(`[INFO] Starting ${options.calls} calls at ${options.callRate} calls/sec`);
    setTimeout(spawnNextCall, 1000);
}
