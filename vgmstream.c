#include <stdio.h>
#include <string.h>
#include <math.h>
#include "vgmstream.h"
#include "meta.h"
#include "layout.h"
#include "coding.h"

static void try_dual_file_stereo(VGMSTREAM * opened_vgmstream, STREAMFILE *streamFile, VGMSTREAM* (*init_vgmstream_function)(STREAMFILE*));


/* List of functions that will recognize files */
VGMSTREAM * (*init_vgmstream_functions[])(STREAMFILE *streamFile) = {
#ifdef VGM_USE_VORBIS
    init_vgmstream_ogg_vorbis,
#endif								     
    init_vgmstream_wwise,
};


/* internal version with all parameters */
static VGMSTREAM * init_vgmstream_internal(STREAMFILE *streamFile) {
    int i, fcns_size;
    
    if (!streamFile)
        return NULL;

    fcns_size = (sizeof(init_vgmstream_functions)/sizeof(init_vgmstream_functions[0]));
    /* try a series of formats, see which works */
    for (i=0; i < fcns_size; i++) {
        /* call init function and see if valid VGMSTREAM was returned */
        VGMSTREAM * vgmstream = (init_vgmstream_functions[i])(streamFile);
        if (!vgmstream)
            continue;

        /* fail if there is nothing to play (without this check vgmstream can generate empty files) */
        if (vgmstream->num_samples <= 0) {
            VGM_LOG("VGMSTREAM: wrong num_samples (ns=%i / 0x%08x)\n", vgmstream->num_samples, vgmstream->num_samples);
            close_vgmstream(vgmstream);
            continue;
        }

        /* everything should have a reasonable sample rate (300 is Wwise min) */
        if (vgmstream->sample_rate < 300 || vgmstream->sample_rate > 96000) {
            VGM_LOG("VGMSTREAM: wrong sample rate (sr=%i)\n", vgmstream->sample_rate);
            close_vgmstream(vgmstream);
            continue;
        }
            
        /* Sanify loops! */
        if (vgmstream->loop_flag) {
            if ((vgmstream->loop_end_sample <= vgmstream->loop_start_sample)
                    || (vgmstream->loop_end_sample > vgmstream->num_samples)
                    || (vgmstream->loop_start_sample < 0) ) {
                vgmstream->loop_flag = 0;
                VGM_LOG("VGMSTREAM: wrong loops ignored (lss=%i, lse=%i, ns=%i)\n", vgmstream->loop_start_sample, vgmstream->loop_end_sample, vgmstream->num_samples);
            }
        }

        /* test if candidate for dual stereo */
        if (vgmstream->channels == 1 && vgmstream->allow_dual_stereo == 1) {
            try_dual_file_stereo(vgmstream, streamFile, init_vgmstream_functions[i]);
        }

        /* files can have thousands subsongs, but let's put a limit */
        if (vgmstream->num_streams < 0 || vgmstream->num_streams > 65535) {
            VGM_LOG("VGMSTREAM: wrong num_streams (ns=%i)\n", vgmstream->num_streams);
            close_vgmstream(vgmstream);
            continue;
        }


        /* save info */
        /* stream_index 0 may be used by plugins to signal "vgmstream default" (IOW don't force to 1) */
        if (!vgmstream->stream_index)
            vgmstream->stream_index = streamFile->stream_index;

        /* save start things so we can restart for seeking */
        memcpy(vgmstream->start_ch,vgmstream->ch,sizeof(VGMSTREAMCHANNEL)*vgmstream->channels);
        memcpy(vgmstream->start_vgmstream,vgmstream,sizeof(VGMSTREAM));

        return vgmstream;
    }

    /* not supported */
    return NULL;
}

/* format detection and VGMSTREAM setup, uses default parameters */
VGMSTREAM * init_vgmstream(const char * const filename) {
    VGMSTREAM *vgmstream = NULL;
    STREAMFILE *streamFile = open_stdio_streamfile(filename);
    if (streamFile) {
        vgmstream = init_vgmstream_from_STREAMFILE(streamFile);
        close_streamfile(streamFile);
    }
    return vgmstream;
}

VGMSTREAM * init_vgmstream_from_STREAMFILE(STREAMFILE *streamFile) {
    return init_vgmstream_internal(streamFile);
}

/* Reset a VGMSTREAM to its state at the start of playback
 * (when a plugin needs to seek back to zero, for instance).
 * Note that this does not reset the constituent STREAMFILES. */
void reset_vgmstream(VGMSTREAM * vgmstream) {
    /* copy the vgmstream back into itself */
    memcpy(vgmstream,vgmstream->start_vgmstream,sizeof(VGMSTREAM));

    /* copy the initial channels */
    memcpy(vgmstream->ch,vgmstream->start_ch,sizeof(VGMSTREAMCHANNEL)*vgmstream->channels);

    /* loop_ch is not zeroed here because there is a possibility of the
     * init_vgmstream_* function doing something tricky and precomputing it.
     * Otherwise hit_loop will be 0 and it will be copied over anyway when we
     * really hit the loop start. */

#ifdef VGM_USE_VORBIS
    if (vgmstream->coding_type==coding_OGG_VORBIS) {
        reset_ogg_vorbis(vgmstream);
    }

    if (vgmstream->coding_type==coding_VORBIS_custom) {
        reset_vorbis_custom(vgmstream);
    }
#endif

    if (vgmstream->layout_type==layout_aix) {
        aix_codec_data *data = vgmstream->codec_data;
        int i;

        data->current_segment = 0;
        for (i=0;i<data->segment_count*data->stream_count;i++) {
            reset_vgmstream(data->adxs[i]);
        }
    }

    if (vgmstream->layout_type==layout_segmented) {
        reset_layout_segmented(vgmstream->layout_data);
    }

    if (vgmstream->layout_type==layout_layered) {
        reset_layout_layered(vgmstream->layout_data);
    }
}

/* Allocate memory and setup a VGMSTREAM */
VGMSTREAM * allocate_vgmstream(int channel_count, int looped) {
    VGMSTREAM * vgmstream;
    VGMSTREAM * start_vgmstream;
    VGMSTREAMCHANNEL * channels;
    VGMSTREAMCHANNEL * start_channels;
    VGMSTREAMCHANNEL * loop_channels;

    /* up to ~16 aren't too rare for multilayered files, more is probably a bug */
    if (channel_count <= 0 || channel_count > 64) {
        VGM_LOG("VGMSTREAM: error allocating %i channels\n", channel_count);
        return NULL;
    }

    vgmstream = calloc(1,sizeof(VGMSTREAM));
    if (!vgmstream) return NULL;
    
    vgmstream->ch = NULL;
    vgmstream->start_ch = NULL;
    vgmstream->loop_ch = NULL;
    vgmstream->start_vgmstream = NULL;
    vgmstream->codec_data = NULL;

    start_vgmstream = calloc(1,sizeof(VGMSTREAM));
    if (!start_vgmstream) {
        free(vgmstream);
        return NULL;
    }
    vgmstream->start_vgmstream = start_vgmstream;
    start_vgmstream->start_vgmstream = start_vgmstream;

    channels = calloc(channel_count,sizeof(VGMSTREAMCHANNEL));
    if (!channels) {
        free(vgmstream);
        free(start_vgmstream);
        return NULL;
    }
    vgmstream->ch = channels;
    vgmstream->channels = channel_count;

    start_channels = calloc(channel_count,sizeof(VGMSTREAMCHANNEL));
    if (!start_channels) {
        free(vgmstream);
        free(start_vgmstream);
        free(channels);
        return NULL;
    }
    vgmstream->start_ch = start_channels;

    if (looped) {
        loop_channels = calloc(channel_count,sizeof(VGMSTREAMCHANNEL));
        if (!loop_channels) {
            free(vgmstream);
            free(start_vgmstream);
            free(channels);
            free(start_channels);
            return NULL;
        }
        vgmstream->loop_ch = loop_channels;
    }

    vgmstream->loop_flag = looped;

    return vgmstream;
}

void close_vgmstream(VGMSTREAM * vgmstream) {
    if (!vgmstream)
        return;

#ifdef VGM_USE_VORBIS
    if (vgmstream->coding_type==coding_OGG_VORBIS) {
        free_ogg_vorbis(vgmstream->codec_data);
        vgmstream->codec_data = NULL;
    }

    if (vgmstream->coding_type==coding_VORBIS_custom) {
        free_vorbis_custom(vgmstream->codec_data);
        vgmstream->codec_data = NULL;
    }
#endif

    if (vgmstream->layout_type==layout_aix) {
        aix_codec_data *data = (aix_codec_data *) vgmstream->codec_data;

        if (data) {
            if (data->adxs) {
                int i;
                for (i=0;i<data->segment_count*data->stream_count;i++) {
                    /* note that the close_streamfile won't do anything but deallocate itself,
                     * there is only one open file in vgmstream->ch[0].streamfile */
                    close_vgmstream(data->adxs[i]);
                }
                free(data->adxs);
            }
            if (data->sample_counts) {
                free(data->sample_counts);
            }

            free(data);
        }
        vgmstream->codec_data = NULL;
    }

    if (vgmstream->layout_type==layout_segmented) {
        free_layout_segmented(vgmstream->layout_data);
        vgmstream->layout_data = NULL;
    }

    if (vgmstream->layout_type==layout_layered) {
        free_layout_layered(vgmstream->layout_data);
        vgmstream->layout_data = NULL;
    }


    /* now that the special cases have had their chance, clean up the standard items */
    {
        int i,j;

        for (i=0;i<vgmstream->channels;i++) {
            if (vgmstream->ch[i].streamfile) {
                close_streamfile(vgmstream->ch[i].streamfile);
                /* Multiple channels might have the same streamfile. Find the others
                 * that are the same as this and clear them so they won't be closed
                 * again. */
                for (j=0;j<vgmstream->channels;j++) {
                    if (i!=j && vgmstream->ch[j].streamfile ==
                                vgmstream->ch[i].streamfile) {
                        vgmstream->ch[j].streamfile = NULL;
                    }
                }
                vgmstream->ch[i].streamfile = NULL;
            }
        }
    }

    if (vgmstream->loop_ch) free(vgmstream->loop_ch);
    if (vgmstream->start_ch) free(vgmstream->start_ch);
    if (vgmstream->ch) free(vgmstream->ch);
    /* the start_vgmstream is considered just data */
    if (vgmstream->start_vgmstream) free(vgmstream->start_vgmstream);

    free(vgmstream);
}

/* calculate samples based on player's config */
int32_t get_vgmstream_play_samples(double looptimes, double fadeseconds, double fadedelayseconds, VGMSTREAM * vgmstream) {
    if (vgmstream->loop_flag) {
        if (vgmstream->loop_target == (int)looptimes) { /* set externally, as this function is info-only */
            /* Continue playing the file normally after looping, instead of fading.
             * Most files cut abruply after the loop, but some do have proper endings.
             * With looptimes = 1 this option should give the same output vs loop disabled */
            int loop_count = (int)looptimes; /* no half loops allowed */
            return vgmstream->loop_start_sample
                + (vgmstream->loop_end_sample - vgmstream->loop_start_sample) * loop_count
                + (vgmstream->num_samples - vgmstream->loop_end_sample);
        }
        else {
            return vgmstream->loop_start_sample
                + (vgmstream->loop_end_sample - vgmstream->loop_start_sample) * looptimes
                + (fadedelayseconds + fadeseconds) * vgmstream->sample_rate;
        }
    }
    else {
        return vgmstream->num_samples;
    }
}

void vgmstream_force_loop(VGMSTREAM* vgmstream, int loop_flag, int loop_start_sample, int loop_end_sample) {
    if (!vgmstream) return;

    /* this requires a bit more messing with the VGMSTREAM than I'm comfortable with... */
    if (loop_flag && !vgmstream->loop_flag && !vgmstream->loop_ch) {
        vgmstream->loop_ch = calloc(vgmstream->channels,sizeof(VGMSTREAMCHANNEL));
        /* loop_ch will be populated when decoded samples reach loop start */
    }
    else if (!loop_flag && vgmstream->loop_flag) {
        /* not important though */
        free(vgmstream->loop_ch);
        vgmstream->loop_ch = NULL;
    }

    vgmstream->loop_flag = loop_flag;
    if (loop_flag) {
        vgmstream->loop_start_sample = loop_start_sample;
        vgmstream->loop_end_sample = loop_end_sample;
    } else {
        vgmstream->loop_start_sample = 0;
        vgmstream->loop_end_sample = 0;
    }

    /* propagate changes to layouts that need them */
    if (vgmstream->layout_type == layout_layered) {
        int i;
        layered_layout_data *data = vgmstream->layout_data;
        for (i = 0; i < data->layer_count; i++) {
            vgmstream_force_loop(data->layers[i], loop_flag, loop_start_sample, loop_end_sample);
        }
    }
    /* segmented layout only works (ATM) with exact/header loop, full loop or no loop */
}

void vgmstream_set_loop_target(VGMSTREAM* vgmstream, int loop_target) {
    if (!vgmstream) return;

    vgmstream->loop_target = loop_target; /* loop count must be rounded (int) as otherwise target is meaningless */

    /* propagate changes to layouts that need them */
    if (vgmstream->layout_type == layout_layered) {
        int i;
        layered_layout_data *data = vgmstream->layout_data;
        for (i = 0; i < data->layer_count; i++) {
            vgmstream_set_loop_target(data->layers[i], loop_target);
        }
    }
}


/* Decode data into sample buffer */
void render_vgmstream(sample * buffer, int32_t sample_count, VGMSTREAM * vgmstream) {
    switch (vgmstream->layout_type) {
        case layout_interleave:
            render_vgmstream_interleave(buffer,sample_count,vgmstream);
            break;
        case layout_none:
            render_vgmstream_flat(buffer,sample_count,vgmstream);
            break;
        case layout_blocked_mxch:
        case layout_blocked_ast:
        case layout_blocked_halpst:
        case layout_blocked_xa:
        case layout_blocked_ea_schl:
        case layout_blocked_ea_1snh:
        case layout_blocked_caf:
        case layout_blocked_wsi:
        case layout_blocked_str_snds:
        case layout_blocked_ws_aud:
        case layout_blocked_matx:
        case layout_blocked_dec:
        case layout_blocked_vs:
        case layout_blocked_emff_ps2:
        case layout_blocked_emff_ngc:
        case layout_blocked_gsb:
        case layout_blocked_xvas:
        case layout_blocked_thp:
        case layout_blocked_filp:
        case layout_blocked_ivaud:
        case layout_blocked_ea_swvr:
        case layout_blocked_adm:
        case layout_blocked_bdsp:
        case layout_blocked_tra:
        case layout_blocked_ps2_iab:
        case layout_blocked_ps2_strlr:
        case layout_blocked_rws:
        case layout_blocked_hwas:
        case layout_blocked_ea_sns:
        case layout_blocked_awc:
        case layout_blocked_vgs:
        case layout_blocked_vawx:
        case layout_blocked_xvag_subsong:
        case layout_blocked_ea_wve_au00:
        case layout_blocked_ea_wve_ad10:
        case layout_blocked_sthd:
        case layout_blocked_h4m:
        case layout_segmented:
            render_vgmstream_segmented(buffer,sample_count,vgmstream);
            break;
        case layout_layered:
            render_vgmstream_layered(buffer,sample_count,vgmstream);
            break;
        default:
            break;
    }


    /* swap channels if set, to create custom channel mappings */
    if (vgmstream->channel_mappings_on) {
        int ch_from,ch_to,s;
        sample temp;
        for (s = 0; s < sample_count; s++) {
            for (ch_from = 0; ch_from < vgmstream->channels; ch_from++) {
                if (ch_from > 32)
                    continue;

                ch_to = vgmstream->channel_mappings[ch_from];
                if (ch_to < 1 || ch_to > 32 || ch_to > vgmstream->channels-1 || ch_from == ch_to)
                    continue;

                temp = buffer[s*vgmstream->channels + ch_from];
                buffer[s*vgmstream->channels + ch_from] = buffer[s*vgmstream->channels + ch_to];
                buffer[s*vgmstream->channels + ch_to] = temp;
            }
        }
    }

    /* channel bitmask to silence non-set channels (up to 32)
     * can be used for 'crossfading subsongs' or layered channels, where a set of channels make a song section */
    if (vgmstream->channel_mask) {
        int ch,s;
        for (s = 0; s < sample_count; s++) {
            for (ch = 0; ch < vgmstream->channels; ch++) {
                if ((vgmstream->channel_mask >> ch) & 1)
                    continue;
                buffer[s*vgmstream->channels + ch] = 0;
            }
        }
    }
}

/* Get the number of samples of a single frame (smallest self-contained sample group, 1/N channels) */
int get_vgmstream_samples_per_frame(VGMSTREAM * vgmstream) {
    switch (vgmstream->coding_type) {
        case coding_CRI_ADX:
        case coding_CRI_ADX_fixed:
        case coding_CRI_ADX_exp:
        case coding_CRI_ADX_enc_8:
        case coding_CRI_ADX_enc_9:
            return (vgmstream->interleave_block_size - 2) * 2;

        case coding_NGC_DSP:
        case coding_NGC_DSP_subint:
            return 14;
        case coding_NGC_AFC:
            return 16;
        case coding_NGC_DTK:
            return 28;
        case coding_G721:
            return 1;

        case coding_PCM16LE:
        case coding_PCM16BE:
        case coding_PCM16_int:
        case coding_PCM8:
        case coding_PCM8_int:
        case coding_PCM8_U:
        case coding_PCM8_U_int:
        case coding_PCM8_SB:
        case coding_ULAW:
        case coding_ULAW_int:
        case coding_ALAW:
        case coding_PCMFLOAT:
            return 1;
#ifdef VGM_USE_VORBIS
        case coding_OGG_VORBIS:
        case coding_VORBIS_custom:
#endif
        case coding_SDX2:
        case coding_SDX2_int:
        case coding_CBD2:
        case coding_ACM:
        case coding_DERF:
        case coding_NWA:
        case coding_SASSC:
            return 1;

        case coding_IMA:
        case coding_DVI_IMA:
        case coding_SNDS_IMA:
        case coding_OTNS_IMA:
        case coding_UBI_IMA:
            return 1;
        case coding_IMA_int:
        case coding_DVI_IMA_int:
        case coding_3DS_IMA:
        case coding_WV6_IMA:
        case coding_ALP_IMA:
        case coding_FFTA2_IMA:
            return 2;
        case coding_XBOX_IMA:
        case coding_XBOX_IMA_mch:
        case coding_XBOX_IMA_int:
        case coding_FSB_IMA:
        case coding_WWISE_IMA:
            return 64;
        case coding_APPLE_IMA4:
            return 64;
        case coding_MS_IMA:
        case coding_REF_IMA:
            return ((vgmstream->interleave_block_size - 0x04*vgmstream->channels) * 2 / vgmstream->channels) + 1;
        case coding_RAD_IMA:
            return (vgmstream->interleave_block_size - 0x04*vgmstream->channels) * 2 / vgmstream->channels;
        case coding_NDS_IMA:
        case coding_DAT4_IMA:
            return (vgmstream->interleave_block_size - 0x04) * 2;
        case coding_AWC_IMA:
            return (0x800 - 0x04) * 2;
        case coding_RAD_IMA_mono:
            return 32;
        case coding_H4M_IMA:
            return 0; /* variable (block-controlled) */

        case coding_XA:
            return 28*8 / vgmstream->channels; /* 8 subframes per frame, mono/stereo */
        case coding_PSX:
        case coding_PSX_badflags:
        case coding_HEVAG:
            return 28;
        case coding_PSX_cfg:
            return (vgmstream->interleave_block_size - 1) * 2; /* decodes 1 byte into 2 bytes */

        case coding_EA_XA:
        case coding_EA_XA_int:
        case coding_EA_XA_V2:
        case coding_MAXIS_XA:
            return 28;
        case coding_EA_XAS:
            return 128;

        case coding_MSADPCM:
            return (vgmstream->interleave_block_size - 0x07*vgmstream->channels)*2 / vgmstream->channels + 2;
        case coding_MSADPCM_ck:
            return (vgmstream->interleave_block_size - 0x07)*2 + 2;
        case coding_WS: /* only works if output sample size is 8 bit, which always is for WS ADPCM */
            return vgmstream->ws_output_size;
        case coding_AICA:
            return 1;
        case coding_AICA_int:
            return 2;
        case coding_YAMAHA:
            return (0x40-0x04*vgmstream->channels) * 2 / vgmstream->channels;
        case coding_YAMAHA_NXAP:
            return (0x40-0x04) * 2;
        case coding_NDS_PROCYON:
            return 30;
        case coding_L5_555:
            return 32;
        case coding_LSF:
            return 54;

#ifdef VGM_USE_G7221
        case coding_G7221C:
            return 32000/50; /* Siren7: 16000/50 */
#endif
#ifdef VGM_USE_G719
        case coding_G719:
            return 48000/50;
#endif
#ifdef VGM_USE_FFMPEG
        case coding_FFmpeg:
            if (vgmstream->codec_data) {
                ffmpeg_codec_data *data = (ffmpeg_codec_data*)vgmstream->codec_data;
                return data->sampleBufferBlock; /* must know the full block size for edge loops */
            }
            else {
                return 0;
            }
            break;
#endif
        case coding_MTAF:
            return 128*2;
        case coding_MTA2:
            return 128*2;
        case coding_MC3:
            return 10;
        case coding_FADPCM:
            return 256; /* (0x8c - 0xc) * 2 */
        case coding_ASF:
            return 32;  /* (0x11 - 0x1) * 2 */
        case coding_XMD:
            return (vgmstream->interleave_block_size - 0x06)*2 + 2;
        case coding_EA_MT:
            return 0; /* 432, but variable in looped files */
        case coding_CRI_HCA:
            return 0; /* 1024 - delay/padding (which can be bigger than 1024) */
#if defined(VGM_USE_MP4V2) && defined(VGM_USE_FDKAAC)
        case coding_MP4_AAC:
            return ((mp4_aac_codec_data*)vgmstream->codec_data)->samples_per_frame;
#endif
#ifdef VGM_USE_MAIATRAC3PLUS
        case coding_AT3plus:
            return 2048 - ((maiatrac3plus_codec_data*)vgmstream->codec_data)->samples_discard;
#endif
#ifdef VGM_USE_ATRAC9
        case coding_ATRAC9:
            return 0; /* varies with config data, usually 256 or 1024 */
#endif
#ifdef VGM_USE_CELT
        case coding_CELT_FSB:
            return 0; /* 512? */
#endif
        default:
            return 0;
    }
}

/* Get the number of bytes of a single frame (smallest self-contained byte group, 1/N channels) */
int get_vgmstream_frame_size(VGMSTREAM * vgmstream) {
    switch (vgmstream->coding_type) {
        case coding_CRI_ADX:
        case coding_CRI_ADX_fixed:
        case coding_CRI_ADX_exp:
        case coding_CRI_ADX_enc_8:
        case coding_CRI_ADX_enc_9:
            return vgmstream->interleave_block_size;

        case coding_NGC_DSP:
            return 0x08;
        case coding_NGC_DSP_subint:
            return 0x08 * vgmstream->channels;
        case coding_NGC_AFC:
            return 0x09;
        case coding_NGC_DTK:
            return 0x20;
        case coding_G721:
            return 0;

        case coding_PCM16LE:
        case coding_PCM16BE:
        case coding_PCM16_int:
            return 0x02;
        case coding_PCM8:
        case coding_PCM8_int:
        case coding_PCM8_U:
        case coding_PCM8_U_int:
        case coding_PCM8_SB:
        case coding_ULAW:
        case coding_ULAW_int:
        case coding_ALAW:
            return 0x01;
        case coding_PCMFLOAT:
            return 0x04;

        case coding_SDX2:
        case coding_SDX2_int:
        case coding_CBD2:
        case coding_DERF:
        case coding_NWA:
        case coding_SASSC:
            return 0x01;

        case coding_IMA:
        case coding_IMA_int:
        case coding_DVI_IMA:
        case coding_DVI_IMA_int:
        case coding_3DS_IMA:
        case coding_WV6_IMA:
        case coding_ALP_IMA:
        case coding_FFTA2_IMA:
            return 0x01;
        case coding_MS_IMA:
        case coding_RAD_IMA:
        case coding_NDS_IMA:
        case coding_DAT4_IMA:
        case coding_REF_IMA:
            return vgmstream->interleave_block_size;
        case coding_AWC_IMA:
            return 0x800;
        case coding_RAD_IMA_mono:
            return 0x14;
        case coding_SNDS_IMA:
        case coding_OTNS_IMA:
            return 0; //todo: 0x01?
        case coding_UBI_IMA: /* variable (PCM then IMA) */
            return 0;
        case coding_XBOX_IMA:
            //todo should be  0x48 when stereo, but blocked/interleave layout don't understand stereo codecs
            return 0x24; //vgmstream->channels==1 ? 0x24 : 0x48;
        case coding_XBOX_IMA_int:
        case coding_WWISE_IMA:
            return 0x24;
        case coding_XBOX_IMA_mch:
        case coding_FSB_IMA:
            return 0x24 * vgmstream->channels;
        case coding_APPLE_IMA4:
            return 0x22;
        case coding_H4M_IMA:
            return 0x00; /* variable (block-controlled) */

        case coding_XA:
            return 0x80;
        case coding_PSX:
        case coding_PSX_badflags:
        case coding_HEVAG:
            return 0x10;
        case coding_PSX_cfg:
            return vgmstream->interleave_block_size;

        case coding_EA_XA:
            return 0x1E;
        case coding_EA_XA_int:
            return 0x0F;
        case coding_MAXIS_XA:
            return 0x0F*vgmstream->channels;
        case coding_EA_XA_V2:
            return 0; /* variable (ADPCM frames of 0x0f or PCM frames of 0x3d) */
        case coding_EA_XAS:
            return 0x4c*vgmstream->channels;

        case coding_MSADPCM:
        case coding_MSADPCM_ck:
            return vgmstream->interleave_block_size;
        case coding_WS:
            return vgmstream->current_block_size;
        case coding_AICA:
        case coding_AICA_int:
            return 0x01;
        case coding_YAMAHA:
        case coding_YAMAHA_NXAP:
            return 0x40;
        case coding_NDS_PROCYON:
            return 0x10;
        case coding_L5_555:
            return 0x12;
        case coding_LSF:
            return 0x1C;

#ifdef VGM_USE_G7221
        case coding_G7221C:
#endif
#ifdef VGM_USE_G719
        case coding_G719:
#endif
#ifdef VGM_USE_MAIATRAC3PLUS
        case coding_AT3plus:
#endif
#ifdef VGM_USE_FFMPEG
        case coding_FFmpeg:
#endif
        case coding_MTAF:
            return vgmstream->interleave_block_size;
        case coding_MTA2:
            return 0x90;
        case coding_MC3:
            return 0x04;
        case coding_FADPCM:
            return 0x8c;
        case coding_ASF:
            return 0x11;
        case coding_XMD:
            return vgmstream->interleave_block_size;
        case coding_EA_MT:
            return 0; /* variable (frames of bit counts or PCM frames) */
#ifdef VGM_USE_ATRAC9
        case coding_ATRAC9:
            return 0; /* varies with config data, usually 0x100-200 */
#endif
#ifdef VGM_USE_CELT
        case coding_CELT_FSB:
            return 0; /* varies, usually 0x80-100 */
#endif
        default: /* Vorbis, MPEG, ACM, etc */
            return 0;
    }
}

/* In NDS IMA the frame size is the block size, so the last one is short */
int get_vgmstream_samples_per_shortframe(VGMSTREAM * vgmstream) {
    switch (vgmstream->coding_type) {
        case coding_NDS_IMA:
            return (vgmstream->interleave_last_block_size-4)*2;
        default:
            return get_vgmstream_samples_per_frame(vgmstream);
    }
}
int get_vgmstream_shortframe_size(VGMSTREAM * vgmstream) {
    switch (vgmstream->coding_type) {
        case coding_NDS_IMA:
            return vgmstream->interleave_last_block_size;
        default:
            return get_vgmstream_frame_size(vgmstream);
    }
}

/* Decode samples into the buffer. Assume that we have written samples_written into the
 * buffer already, and we have samples_to_do consecutive samples ahead of us. */
void decode_vgmstream(VGMSTREAM * vgmstream, int samples_written, int samples_to_do, sample * buffer) {
    switch (vgmstream->coding_type) {
#ifdef VGM_USE_VORBIS
        case coding_OGG_VORBIS:
            decode_ogg_vorbis(vgmstream->codec_data, buffer+samples_written*vgmstream->channels,
                    samples_to_do,vgmstream->channels);
            break;

        case coding_VORBIS_custom:
            decode_vorbis_custom(vgmstream, buffer+samples_written*vgmstream->channels,
                    samples_to_do,vgmstream->channels);
            break;
#endif
        default:
            break;
    }
}

/* Calculate number of consecutive samples to do (taking into account stopping for loop start and end) */
int vgmstream_samples_to_do(int samples_this_block, int samples_per_frame, VGMSTREAM * vgmstream) {
    int samples_to_do;
    int samples_left_this_block;

    samples_left_this_block = samples_this_block - vgmstream->samples_into_block;
    samples_to_do = samples_left_this_block;

    /* fun loopy crap */
    /* Why did I think this would be any simpler? */
    if (vgmstream->loop_flag) {
        /* are we going to hit the loop end during this block? */
        if (vgmstream->current_sample+samples_left_this_block > vgmstream->loop_end_sample) {
            /* only do to just before it */
            samples_to_do = vgmstream->loop_end_sample-vgmstream->current_sample;
        }

        /* are we going to hit the loop start during this block? */
        if (!vgmstream->hit_loop && vgmstream->current_sample+samples_left_this_block > vgmstream->loop_start_sample) {
            /* only do to just before it */
            samples_to_do = vgmstream->loop_start_sample-vgmstream->current_sample;
        }

    }

    /* if it's a framed encoding don't do more than one frame */
    if (samples_per_frame>1 && (vgmstream->samples_into_block%samples_per_frame)+samples_to_do>samples_per_frame)
        samples_to_do = samples_per_frame - (vgmstream->samples_into_block%samples_per_frame);

    return samples_to_do;
}

/* Detect loop start and save values, or detect loop end and restore (loop back). Returns 1 if loop was done. */
int vgmstream_do_loop(VGMSTREAM * vgmstream) {
    /*if (!vgmstream->loop_flag) return 0;*/

    /* is this the loop end? = new loop, continue from loop_start_sample */
    if (vgmstream->current_sample==vgmstream->loop_end_sample) {

        /* disable looping if target count reached and continue normally
         * (only needed with the "play stream end after looping N times" option enabled) */
        vgmstream->loop_count++;
        if (vgmstream->loop_target && vgmstream->loop_target == vgmstream->loop_count) {
            vgmstream->loop_flag = 0; /* could be improved but works ok */
            return 0;
        }

        /* against everything I hold sacred, preserve adpcm
         * history through loop for certain types */
        if (vgmstream->meta_type == meta_DSP_STD ||
            vgmstream->meta_type == meta_DSP_RS03 ||
            vgmstream->meta_type == meta_DSP_CSTR ||
            vgmstream->coding_type == coding_PSX ||
            vgmstream->coding_type == coding_PSX_badflags) {
            int i;
            for (i=0;i<vgmstream->channels;i++) {
                vgmstream->loop_ch[i].adpcm_history1_16 = vgmstream->ch[i].adpcm_history1_16;
                vgmstream->loop_ch[i].adpcm_history2_16 = vgmstream->ch[i].adpcm_history2_16;
                vgmstream->loop_ch[i].adpcm_history1_32 = vgmstream->ch[i].adpcm_history1_32;
                vgmstream->loop_ch[i].adpcm_history2_32 = vgmstream->ch[i].adpcm_history2_32;
            }
        }


        /* prepare certain codecs' internal state for looping */

#ifdef VGM_USE_VORBIS
        if (vgmstream->coding_type==coding_OGG_VORBIS) {
            seek_ogg_vorbis(vgmstream, vgmstream->loop_sample);
        }

        if (vgmstream->coding_type==coding_VORBIS_custom) {
            seek_vorbis_custom(vgmstream, vgmstream->loop_start_sample);
        }
#endif

#ifdef VGM_USE_FFMPEG
        if (vgmstream->coding_type==coding_FFmpeg) {
            seek_ffmpeg(vgmstream, vgmstream->loop_start_sample);
        }
#endif

#if defined(VGM_USE_MP4V2) && defined(VGM_USE_FDKAAC)
        if (vgmstream->coding_type==coding_MP4_AAC) {
            seek_mp4_aac(vgmstream, vgmstream->loop_sample);
        }
#endif

#ifdef VGM_USE_MAIATRAC3PLUS
        if (vgmstream->coding_type==coding_AT3plus) {
            seek_at3plus(vgmstream, vgmstream->loop_sample);
        }
#endif

#ifdef VGM_USE_ATRAC9
        if (vgmstream->coding_type==coding_ATRAC9) {
            seek_atrac9(vgmstream, vgmstream->loop_sample);
        }
#endif

#ifdef VGM_USE_CELT
        if (vgmstream->coding_type==coding_CELT_FSB) {
            seek_celt_fsb(vgmstream, vgmstream->loop_sample);
        }
#endif

#ifdef VGM_USE_MPEG
        if (vgmstream->coding_type==coding_MPEG_custom ||
            vgmstream->coding_type==coding_MPEG_ealayer3 ||
            vgmstream->coding_type==coding_MPEG_layer1 ||
            vgmstream->coding_type==coding_MPEG_layer2 ||
            vgmstream->coding_type==coding_MPEG_layer3) {
            seek_mpeg(vgmstream, vgmstream->loop_sample);
        }
#endif

        /* restore! */
        memcpy(vgmstream->ch,vgmstream->loop_ch,sizeof(VGMSTREAMCHANNEL)*vgmstream->channels);
        vgmstream->current_sample = vgmstream->loop_sample;
        vgmstream->samples_into_block = vgmstream->loop_samples_into_block;
        vgmstream->current_block_size = vgmstream->loop_block_size;
        vgmstream->current_block_samples = vgmstream->loop_block_samples;
        vgmstream->current_block_offset = vgmstream->loop_block_offset;
        vgmstream->next_block_offset = vgmstream->loop_next_block_offset;

        return 1; /* looped */
    }


    /* is this the loop start? */
    if (!vgmstream->hit_loop && vgmstream->current_sample==vgmstream->loop_start_sample) {
        /* save! */
        memcpy(vgmstream->loop_ch,vgmstream->ch,sizeof(VGMSTREAMCHANNEL)*vgmstream->channels);

        vgmstream->loop_sample = vgmstream->current_sample;
        vgmstream->loop_samples_into_block = vgmstream->samples_into_block;
        vgmstream->loop_block_size = vgmstream->current_block_size;
        vgmstream->loop_block_samples = vgmstream->current_block_samples;
        vgmstream->loop_block_offset = vgmstream->current_block_offset;
        vgmstream->loop_next_block_offset = vgmstream->next_block_offset;
        vgmstream->hit_loop = 1;
    }

    return 0; /* not looped */
}

/* Write a description of the stream into array pointed by desc, which must be length bytes long.
 * Will always be null-terminated if length > 0 */
void describe_vgmstream(VGMSTREAM * vgmstream, char * desc, int length) {
#define TEMPSIZE 256
    char temp[TEMPSIZE];
    const char* description;

    if (!vgmstream) {
        snprintf(temp,TEMPSIZE,
                "NULL VGMSTREAM");
        concatn(length,desc,temp);
        return;
    }

    snprintf(temp,TEMPSIZE,
            "sample rate: %d Hz\n"
            "channels: %d\n",
            vgmstream->sample_rate,
            vgmstream->channels);
    concatn(length,desc,temp);

    if (vgmstream->loop_flag) {
        snprintf(temp,TEMPSIZE,
                "loop start: %d samples (%.4f seconds)\n"
                "loop end: %d samples (%.4f seconds)\n",
                vgmstream->loop_start_sample,
                (double)vgmstream->loop_start_sample/vgmstream->sample_rate,
                vgmstream->loop_end_sample,
                (double)vgmstream->loop_end_sample/vgmstream->sample_rate);
        concatn(length,desc,temp);
    }

    snprintf(temp,TEMPSIZE,
            "stream total samples: %d (%.4f seconds)\n",
            vgmstream->num_samples,
            (double)vgmstream->num_samples/vgmstream->sample_rate);
    concatn(length,desc,temp);

    snprintf(temp,TEMPSIZE,
            "encoding: ");
    concatn(length,desc,temp);
    switch (vgmstream->coding_type) {
#ifdef VGM_USE_FFMPEG
        case coding_FFmpeg: {
            ffmpeg_codec_data *data = (ffmpeg_codec_data *)vgmstream->codec_data;
            if (!data && vgmstream->layout_data) {
                layered_layout_data* layout_data = vgmstream->layout_data;
                if (layout_data->layers[0]->coding_type == coding_FFmpeg)
                    data = layout_data->layers[0]->codec_data;
            }

            if (data) {
                if (data->codec && data->codec->long_name) {
                    snprintf(temp,TEMPSIZE,"%s",data->codec->long_name);
                } else if (data->codec && data->codec->name) {
                    snprintf(temp,TEMPSIZE,"%s",data->codec->name);
                } else {
                    snprintf(temp,TEMPSIZE,"FFmpeg (unknown codec)");
                }
            } else {
                snprintf(temp,TEMPSIZE,"FFmpeg");
            }
            break;
        }
#endif
        default:
            description = get_vgmstream_coding_description(vgmstream->coding_type);
            if (!description)
                description = "CANNOT DECODE";
            snprintf(temp,TEMPSIZE,"%s",description);
            break;
    }
    concatn(length,desc,temp);

    snprintf(temp,TEMPSIZE,
            "\nlayout: ");
    concatn(length,desc,temp);
    switch (vgmstream->layout_type) {
        default:
            description = get_vgmstream_layout_description(vgmstream->layout_type);
            if (!description)
                description = "INCONCEIVABLE";
            snprintf(temp,TEMPSIZE,"%s",description);
            break;
    }
    concatn(length,desc,temp);

    snprintf(temp,TEMPSIZE,
            "\n");
    concatn(length,desc,temp);

    if (vgmstream->layout_type == layout_interleave && vgmstream->channels > 1) {
        snprintf(temp,TEMPSIZE,
                "interleave: %#x bytes\n",
                (int32_t)vgmstream->interleave_block_size);
        concatn(length,desc,temp);

        if (vgmstream->interleave_last_block_size) {
            snprintf(temp,TEMPSIZE,
                    "interleave last block: %#x bytes\n",
                    (int32_t)vgmstream->interleave_last_block_size);
            concatn(length,desc,temp);
        }
    }

    /* codecs with configurable frame size */
    if (vgmstream->layout_type == layout_none && vgmstream->interleave_block_size > 0) {
        switch (vgmstream->coding_type) {
            case coding_MSADPCM:
            case coding_MSADPCM_ck:
            case coding_MS_IMA:
            case coding_MC3:
            case coding_WWISE_IMA:
            case coding_REF_IMA:
            case coding_PSX_cfg:
                snprintf(temp,TEMPSIZE,
                        "frame size: %#x bytes\n",
                        (int32_t)vgmstream->interleave_block_size);
                concatn(length,desc,temp);
                break;
            default:
                break;
        }
    }

    snprintf(temp,TEMPSIZE,
            "metadata from: ");
    concatn(length,desc,temp);
    switch (vgmstream->meta_type) {
        default:
            description = get_vgmstream_meta_description(vgmstream->meta_type);
            if (!description)
                description = "THEY SHOULD HAVE SENT A POET";
            snprintf(temp,TEMPSIZE,"%s",description);
            break;
    }
    concatn(length,desc,temp);

    snprintf(temp,TEMPSIZE,
            "\nbitrate: %d kbps",
            get_vgmstream_average_bitrate(vgmstream) / 1000);
    concatn(length,desc,temp);

    /* only interesting if more than one */
    if (vgmstream->num_streams > 1) {
        snprintf(temp,TEMPSIZE,
                "\nstream count: %d",
                vgmstream->num_streams);
        concatn(length,desc,temp);
    }

    if (vgmstream->num_streams > 1 && vgmstream->stream_index > 0) {
        snprintf(temp,TEMPSIZE,
                "\nstream index: %d",
                vgmstream->stream_index);
        concatn(length,desc,temp);
    }
    if (vgmstream->stream_name[0] != '\0') {
        snprintf(temp,TEMPSIZE,
                "%s",
                vgmstream->stream_name);
        concatn(length,desc,temp);
    }
}


/* See if there is a second file which may be the second channel, given an already opened mono vgmstream.
 * If a suitable file is found, open it and change opened_vgmstream to a stereo vgmstream. */
static void try_dual_file_stereo(VGMSTREAM * opened_vgmstream, STREAMFILE *streamFile, VGMSTREAM*(*init_vgmstream_function)(STREAMFILE *)) {
    /* filename search pairs for dual file stereo */
    static const char * const dfs_pairs[][2] = {
        {"L","R"},
        {"l","r"},
        {"left","right"},
        {"Left","Right"},
        {".V0",".V1"}, /* Homura (PS2) */
        {".L",".R"}, /* Crash Nitro Racing (PS2), Gradius V (PS2) */
        {"_0","_1"}, //fake for Homura/unneeded?
    };
    char new_filename[PATH_LIMIT];
    char * ext;
    int dfs_pair = -1; /* -1=no stereo, 0=opened_vgmstream is left, 1=opened_vgmstream is right */
    VGMSTREAM *new_vgmstream = NULL;
    STREAMFILE *dual_streamFile = NULL;
    int i,j, dfs_pair_count;

    if (opened_vgmstream->channels != 1)
        return;

    /* vgmstream's layout stuff currently assumes a single file */
    // fastelbja : no need ... this one works ok with dual file
    //if (opened_vgmstream->layout != layout_none) return;
    //todo force layout_none if layout_interleave?

    streamFile->get_name(streamFile,new_filename,sizeof(new_filename));
    if (strlen(new_filename)<2) return; /* we need at least a base and a name ending to replace */
    
    ext = (char *)filename_extension(new_filename);
    if (ext-new_filename >= 1 && ext[-1]=='.') ext--; /* including "." */

    /* find pair from base name and modify new_filename with the opposite */
    dfs_pair_count = (sizeof(dfs_pairs)/sizeof(dfs_pairs[0]));
    for (i = 0; dfs_pair == -1 && i< dfs_pair_count; i++) {
        for (j=0; dfs_pair==-1 && j<2; j++) {
            const char * this_suffix = dfs_pairs[i][j];
            size_t this_suffix_len = strlen(dfs_pairs[i][j]);
            const char * other_suffix = dfs_pairs[i][j^1];
            size_t other_suffix_len = strlen(dfs_pairs[i][j^1]);

            /* if suffix matches copy opposite to ext pointer (thus to new_filename) */
            if (this_suffix[0] == '.' && strlen(ext) == this_suffix_len) { /* dual extension (ex. Homura PS2) */
                if ( !memcmp(ext,this_suffix,this_suffix_len) ) {
                    dfs_pair = j;
                    memcpy (ext, other_suffix,other_suffix_len); /* overwrite with new extension */
                }
            }
            else { /* dual suffix */
                if ( !memcmp(ext - this_suffix_len,this_suffix,this_suffix_len) ) {
                    dfs_pair = j;
                    memmove(ext + other_suffix_len - this_suffix_len, ext,strlen(ext)+1); /* move the extension and terminator, too */
                    memcpy (ext - this_suffix_len, other_suffix,other_suffix_len); /* overwrite with new suffix */
                }
            }

        }
    }

    /* see if the filename had a suitable L/R-pair name */
    if (dfs_pair == -1)
        goto fail;


    /* try to init other channel (new_filename now has the opposite name) */
    dual_streamFile = streamFile->open(streamFile,new_filename,STREAMFILE_DEFAULT_BUFFER_SIZE);
    if (!dual_streamFile) goto fail;

    new_vgmstream = init_vgmstream_function(dual_streamFile); /* use the init that just worked, no other should work */
    close_streamfile(dual_streamFile);

    /* see if we were able to open the file, and if everything matched nicely */
    if (!(new_vgmstream &&
            new_vgmstream->channels == 1 &&
            /* we have seen legitimate pairs where these are off by one...
             * but leaving it commented out until I can find those and recheck */
            /* abs(new_vgmstream->num_samples-opened_vgmstream->num_samples <= 1) && */
            new_vgmstream->num_samples == opened_vgmstream->num_samples &&
            new_vgmstream->sample_rate == opened_vgmstream->sample_rate &&
            new_vgmstream->meta_type   == opened_vgmstream->meta_type &&
            new_vgmstream->coding_type == opened_vgmstream->coding_type &&
            new_vgmstream->layout_type == opened_vgmstream->layout_type &&
            /* check even if the layout doesn't use them, because it is
             * difficult to determine when it does, and they should be zero otherwise, anyway */
            new_vgmstream->interleave_block_size == opened_vgmstream->interleave_block_size &&
            new_vgmstream->interleave_last_block_size == opened_vgmstream->interleave_last_block_size)) {
        goto fail;
    }

    /* check these even if there is no loop, because they should then be zero in both
     * Homura PS2 right channel doesn't have loop points so it's ignored */
    if (new_vgmstream->meta_type != meta_PS2_SMPL &&
            !(new_vgmstream->loop_flag      == opened_vgmstream->loop_flag &&
            new_vgmstream->loop_start_sample== opened_vgmstream->loop_start_sample &&
            new_vgmstream->loop_end_sample  == opened_vgmstream->loop_end_sample)) {
        goto fail;
    }

    /* We seem to have a usable, matching file. Merge in the second channel. */
    {
        VGMSTREAMCHANNEL * new_chans;
        VGMSTREAMCHANNEL * new_loop_chans = NULL;
        VGMSTREAMCHANNEL * new_start_chans = NULL;

        /* build the channels */
        new_chans = calloc(2,sizeof(VGMSTREAMCHANNEL));
        if (!new_chans) goto fail;

        memcpy(&new_chans[dfs_pair],&opened_vgmstream->ch[0],sizeof(VGMSTREAMCHANNEL));
        memcpy(&new_chans[dfs_pair^1],&new_vgmstream->ch[0],sizeof(VGMSTREAMCHANNEL));

        /* loop and start will be initialized later, we just need to allocate them here */
        new_start_chans = calloc(2,sizeof(VGMSTREAMCHANNEL));
        if (!new_start_chans) {
            free(new_chans);
            goto fail;
        }

        if (opened_vgmstream->loop_ch) {
            new_loop_chans = calloc(2,sizeof(VGMSTREAMCHANNEL));
            if (!new_loop_chans) {
                free(new_chans);
                free(new_start_chans);
                goto fail;
            }
        }

        /* remove the existing structures */
        /* not using close_vgmstream as that would close the file */
        free(opened_vgmstream->ch);
        free(new_vgmstream->ch);

        free(opened_vgmstream->start_ch);
        free(new_vgmstream->start_ch);

        if (opened_vgmstream->loop_ch) {
            free(opened_vgmstream->loop_ch);
            free(new_vgmstream->loop_ch);
        }

        /* fill in the new structures */
        opened_vgmstream->ch = new_chans;
        opened_vgmstream->start_ch = new_start_chans;
        opened_vgmstream->loop_ch = new_loop_chans;

        /* stereo! */
        opened_vgmstream->channels = 2;

        /* discard the second VGMSTREAM */
        free(new_vgmstream);
    }

fail:
    return;
}

/* average bitrate helper to get STREAMFILE for a channel, since some codecs may use their own */
static STREAMFILE * get_vgmstream_average_bitrate_channel_streamfile(VGMSTREAM * vgmstream, int channel) {

    if (vgmstream->coding_type==coding_ACM) {
        acm_codec_data *data = vgmstream->codec_data;
        return (data && data->handle) ? data->streamfile : NULL;
    }

#ifdef VGM_USE_VORBIS
    if (vgmstream->coding_type==coding_OGG_VORBIS) {
        ogg_vorbis_codec_data *data = vgmstream->codec_data;
        return data ? data->ov_streamfile.streamfile : NULL;
    }
#endif
    return vgmstream->ch[channel].streamfile;
}

static int get_vgmstream_average_bitrate_from_size(size_t size, int sample_rate, int length_samples) {
    return (int)((int64_t)size * 8 * sample_rate / length_samples);
}
static int get_vgmstream_average_bitrate_from_streamfile(STREAMFILE * streamfile, int sample_rate, int length_samples) {
    return get_vgmstream_average_bitrate_from_size(get_streamfile_size(streamfile), sample_rate, length_samples);
}

/* Return the average bitrate in bps of all unique files contained within this stream. */
int get_vgmstream_average_bitrate(VGMSTREAM * vgmstream) {
    STREAMFILE *streamfiles[64];
    const size_t streamfiles_max = 64; /* arbitrary max, */
    size_t streamfiles_size = 0;
    size_t streams_size = 0;
    unsigned int ch, sub;

    int bitrate = 0;
    int sample_rate = vgmstream->sample_rate;
    int length_samples = vgmstream->num_samples;

    if (!sample_rate || !length_samples)
        return 0;

    /* subsongs need to report this to properly calculate */
    if (vgmstream->stream_size) {
        return get_vgmstream_average_bitrate_from_size(vgmstream->stream_size, sample_rate, length_samples);
    }


    /* make a list of used streamfiles (repeats will be filtered below) */
    if (vgmstream->layout_type==layout_segmented) {
        segmented_layout_data *data = (segmented_layout_data *) vgmstream->layout_data;
        for (sub = 0; sub < data->segment_count; sub++) {
            streams_size += data->segments[sub]->stream_size;
            for (ch = 0; ch < data->segments[sub]->channels; ch++) {
                if (streamfiles_size >= streamfiles_max) continue;
                streamfiles[streamfiles_size] = get_vgmstream_average_bitrate_channel_streamfile(data->segments[sub], ch);
                streamfiles_size++;
            }
        }
    }
    else if (vgmstream->layout_type==layout_layered) {
        layered_layout_data *data = vgmstream->layout_data;
        for (sub = 0; sub < data->layer_count; sub++) {
            streams_size += data->layers[sub]->stream_size;
            for (ch = 0; ch < data->layers[sub]->channels; ch++) {
                if (streamfiles_size >= streamfiles_max) continue;
                streamfiles[streamfiles_size] = get_vgmstream_average_bitrate_channel_streamfile(data->layers[sub], ch);
                streamfiles_size++;
            }
        }
    }
    else {
        for (ch = 0; ch < vgmstream->channels; ch++) {
            if (streamfiles_size >= streamfiles_max)
                continue;
            streamfiles[streamfiles_size] = get_vgmstream_average_bitrate_channel_streamfile(vgmstream, ch);
            streamfiles_size++;
        }
    }

    /* could have a sum of all sub-VGMSTREAMs */
    if (streams_size) {
        return get_vgmstream_average_bitrate_from_size(streams_size, sample_rate, length_samples);
    }

    /* compare files by absolute paths, so bitrate doesn't multiply when the same STREAMFILE is
     * reopened per channel, also skipping repeated pointers. */
    {
        char path_current[PATH_LIMIT];
        char path_compare[PATH_LIMIT];
        unsigned int i, j;

        for (i = 0; i < streamfiles_size; i++) {
            STREAMFILE * currentFile = streamfiles[i];
            if (!currentFile) continue;
            get_streamfile_name(currentFile, path_current, sizeof(path_current));

            for (j = 0; j < i; j++) {
                STREAMFILE * compareFile = streamfiles[j];
                if (!compareFile) continue;
                if (currentFile == compareFile)
                    break;
                get_streamfile_name(compareFile, path_compare, sizeof(path_compare));
                if (strcmp(path_current, path_compare) == 0)
                    break;
            }

            if (i == j) { /* current STREAMFILE hasn't appeared previously */
                bitrate += get_vgmstream_average_bitrate_from_streamfile(currentFile, sample_rate, length_samples);
            }
        }
    }

    return bitrate;
}


/**
 * Inits vgmstream, doing two things:
 * - sets the starting offset per channel (depending on the layout)
 * - opens its own streamfile from on a base one. One streamfile per channel may be open (to improve read/seeks).
 * Should be called in metas before returning the VGMSTREAM.
 */
int vgmstream_open_stream(VGMSTREAM * vgmstream, STREAMFILE *streamFile, off_t start_offset) {
    STREAMFILE * file = NULL;
    char filename[PATH_LIMIT];
    int ch;
    int use_streamfile_per_channel = 0;
    int use_same_offset_per_channel = 0;
    int is_stereo_codec = 0;


    /* stream/offsets not needed, managed by layout */
    if (vgmstream->layout_type == layout_aix ||
        vgmstream->layout_type == layout_segmented ||
        vgmstream->layout_type == layout_layered)
        return 1;

    /* stream/offsets not needed, managed by decoder */
    if (vgmstream->coding_type == coding_NWA ||
        vgmstream->coding_type == coding_ACM ||
        vgmstream->coding_type == coding_CRI_HCA)
        return 1;

#ifdef VGM_USE_FFMPEG
    /* stream/offsets not needed, managed by decoder */
    if (vgmstream->coding_type == coding_FFmpeg)
        return 1;
#endif

    /* if interleave is big enough keep a buffer per channel */
    if (vgmstream->interleave_block_size * vgmstream->channels >= STREAMFILE_DEFAULT_BUFFER_SIZE) {
        use_streamfile_per_channel = 1;
    }

    /* if blocked layout (implicit) use multiple streamfiles; using only one leads to
     * lots of buffer-trashing, with all the jumping around in the block layout */
    if (vgmstream->layout_type != layout_none && vgmstream->layout_type != layout_interleave) {
        use_streamfile_per_channel = 1;
    }

    /* for mono or codecs like IMA (XBOX, MS IMA, MS ADPCM) where channels work with the same bytes */
    if (vgmstream->layout_type == layout_none) {
        use_same_offset_per_channel = 1;
    }

    /* stereo codecs interleave in 2ch pairs (interleave size should still be: full_block_size / channels) */
    if (vgmstream->layout_type == layout_interleave && vgmstream->coding_type == coding_XBOX_IMA) {
        is_stereo_codec = 1;
    }

    streamFile->get_name(streamFile,filename,sizeof(filename));
    /* open the file for reading by each channel */
    {
        if (!use_streamfile_per_channel) {
            file = streamFile->open(streamFile,filename, STREAMFILE_DEFAULT_BUFFER_SIZE);
            if (!file) goto fail;
        }

        for (ch=0; ch < vgmstream->channels; ch++) {
            off_t offset;
            if (use_same_offset_per_channel) {
                offset = start_offset;
            } else if (is_stereo_codec) {
                int ch_mod = (ch & 1) ? ch - 1 : ch; /* adjust odd channels (ch 0,1,2,3,4,5 > ch 0,0,2,2,4,4) */
                offset = start_offset + vgmstream->interleave_block_size*ch_mod;
                //VGM_LOG("ch%i offset=%lx\n", ch,offset);
            } else {
                offset = start_offset + vgmstream->interleave_block_size*ch;
            }

            /* open new one if needed */
            if (use_streamfile_per_channel) {
                file = streamFile->open(streamFile,filename, STREAMFILE_DEFAULT_BUFFER_SIZE);
                if (!file) goto fail;
            }

            vgmstream->ch[ch].streamfile = file;
            vgmstream->ch[ch].channel_start_offset =
                    vgmstream->ch[ch].offset = offset;
        }
    }

    return 1;

fail:
    /* open streams will be closed in close_vgmstream(), hopefully called by the meta */
    return 0;
}
