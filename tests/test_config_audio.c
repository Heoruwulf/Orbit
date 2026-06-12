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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "orbit/config.h"

int main(void) {
    // Set mock environment variables
    setenv("WS_CODEC_PAYLOAD_TYPE", "PCMU", 1);
    setenv("WS_CODEC_SAMPLE_RATE", "8000", 1);
    setenv("WS_CODEC_CHANNELS", "2", 1);
    setenv("WS_CODEC_ENDIAN", "big", 1);
    setenv("WS_CODEC_VAD_ENABLE", "true", 1);
    setenv("VAD_FILE", "/tmp/vad.bin", 1);

    // Provide required minimum env vars to pass config_load validations if any.
    // In Orbit config.c, default ports are 16000-32000 for RTP, which pass validation.

    int const ret = config_load();
    assert(ret == 0);

    assert(strcmp(g_config.ws_codec_payload_type, "PCMU") == 0);
    assert(g_config.ws_codec_sample_rate == 8000);
    assert(g_config.ws_codec_channels == 2);
    assert(strcmp(g_config.ws_codec_endian, "big") == 0);
    assert(g_config.ws_codec_vad_enable == true);
    assert(strcmp(g_config.vad_file, "/tmp/vad.bin") == 0);

    return EXIT_SUCCESS;
}
