// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/audiosender.h>
#include <string.h>
#include <chiaki/fec.h>

static void chiaki_audio_sender_frame(ChiakiAudioSender *audio_sender, uint8_t *buf, size_t buf_size);
CHIAKI_EXPORT ChiakiErrorCode chiaki_audio_sender_init(ChiakiAudioSender *audio_sender, ChiakiLog *log, ChiakiSession *session)
{
    audio_sender->log = log;
    audio_sender->ps5 = session->connect_info.ps5;
    audio_sender->takion = &(session->stream_connection.takion);
    audio_sender->frame_index = 0;
    audio_sender->buf_size_per_unit = 40;
    audio_sender->buf_stride_per_unit = ((audio_sender->buf_size_per_unit + 0xf) / 0x10) * 0x10;
    audio_sender->frame_buf_size = 3 * audio_sender->buf_size_per_unit;
    audio_sender->frame_buf = malloc(audio_sender->frame_buf_size);
    audio_sender->filled_packet_buf = malloc(audio_sender->frame_buf_size + 20);

    ChiakiErrorCode err = chiaki_mutex_init(&audio_sender->mutex, false);
    if(err != CHIAKI_ERR_SUCCESS)
        return err;

    return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_audio_sender_fini(ChiakiAudioSender *audio_sender)
{
#ifdef CHIAKI_LIB_ENABLE_OPUS
	opus_encoder_destroy(audio_sender->opus_decoder);
#endif
    free(audio_sender->frame_buf);
    free(audio_sender->filled_packet_buf);
    chiaki_mutex_fini(&audio_sender->mutex);
}

CHIAKI_EXPORT void chiaki_audio_sender_opus_data(ChiakiAudioSender *audio_sender, uint8_t *opus_sender, size_t opus_sender_size)
{
    // skip audio packets without encoded audio
    // if no audio the packet will have only 3 encoded units because there is no entropy in the packet, otherwise should be max of 40
    if(opus_sender_size != audio_sender->buf_size_per_unit)
        return;
    uint8_t packet_type = 3; // TAKION_PACKET_TYPE_AUDIO
    uint16_t packet_index = htons(audio_sender->frame_index);
    uint16_t frame_index = htons(audio_sender->frame_index + 1);
    uint32_t unit_index = 0;
    uint32_t units_in_frame_source = 1;
    uint32_t units_in_frame_fec_real = 2;
    uint32_t units_in_frame_total = 3;
    uint32_t units_in_frame_fec_raw = 10273;
    uint32_t units_number = htonl((units_in_frame_fec_raw & 0xffff) | (((units_in_frame_total - 1) & 0xff) << 0x10) | ((unit_index & 0xff) << 0x18));
    uint32_t key_pos = htonl(0);
    uint8_t codec = 5;
    uint32_t gmac = htonl(0);
    uint8_t zero_byte = 0;
    bool is_ps5 = audio_sender->ps5;
    uint8_t ps5_packet = 0;
    if(is_ps5)
        ps5_packet = 1;

    memcpy(audio_sender->frame_buf, opus_sender, opus_sender_size);
    ChiakiErrorCode err = chiaki_fec_encode(audio_sender->frame_buf,
			    audio_sender->buf_size_per_unit, audio_sender->buf_stride_per_unit,
			    units_in_frame_source, units_in_frame_fec_real);

	if(err != CHIAKI_ERR_SUCCESS)
	{
		err = CHIAKI_ERR_FEC_FAILED;
		CHIAKI_LOGE(audio_sender->log, "FEC encoding failed");
	}

    uint32_t filled_packet_size = audio_sender->frame_buf_size + 19 + ps5_packet;

    audio_sender->filled_packet_buf[0] = packet_type;
    *(chiaki_unaligned_uint16_t *)(audio_sender->filled_packet_buf + 1) = packet_index;
    *(chiaki_unaligned_uint16_t *)(audio_sender->filled_packet_buf + 3) = frame_index;
    *(chiaki_unaligned_uint32_t *)(audio_sender->filled_packet_buf + 5) = units_number;
    audio_sender->filled_packet_buf[9] = codec;
    *(chiaki_unaligned_uint32_t *)(audio_sender->filled_packet_buf + 10) = gmac;
    *(chiaki_unaligned_uint32_t *)(audio_sender->filled_packet_buf + 14) = key_pos;
    audio_sender->filled_packet_buf[18] = zero_byte;
    if(is_ps5)
        audio_sender->filled_packet_buf[19] = zero_byte;
    memcpy(audio_sender->filled_packet_buf + 19 + ps5_packet, audio_sender->frame_buf, audio_sender->frame_buf_size);
	chiaki_audio_sender_frame(audio_sender, audio_sender->filled_packet_buf, filled_packet_size);
}

static void chiaki_audio_sender_frame(ChiakiAudioSender *audio_sender, uint8_t *buf, size_t buf_size)
{
	chiaki_mutex_lock(&audio_sender->mutex);
	chiaki_takion_send_mic_packet(audio_sender->takion, buf, buf_size, audio_sender->ps5);
    if (audio_sender->frame_index == UINT16_MAX)
        audio_sender->frame_index = 0;
    else
        audio_sender->frame_index++;
    chiaki_mutex_unlock(&audio_sender->mutex);
}
