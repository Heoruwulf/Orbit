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
