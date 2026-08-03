FATE_API_LIBAVCODEC-$(call ENCDEC, FLAC, FLAC) += fate-api-flac
fate-api-flac: $(APITESTSDIR)/api-flac-test$(EXESUF)
fate-api-flac: CMD = run $(APITESTSDIR)/api-flac-test$(EXESUF)
fate-api-flac: CMP = null

FATE_API_LIBAVCODEC-$(call ALLYES, H261_ENCODER H261_PARSER) += fate-api-enc-parser fate-api-enc-parser-cif
fate-api-enc-parser: $(APITESTSDIR)/api-enc-parser-test$(EXESUF)
fate-api-enc-parser: CMD = run $(APITESTSDIR)/api-enc-parser-test$(EXESUF) h261 176 144
fate-api-enc-parser-cif: $(APITESTSDIR)/api-enc-parser-test$(EXESUF)
fate-api-enc-parser-cif: CMD = run $(APITESTSDIR)/api-enc-parser-test$(EXESUF) h261 352 288

FATE_API_SAMPLES_LIBAVFORMAT-$(call DEMDEC, FLV, FLV) += fate-api-band
fate-api-band: $(APITESTSDIR)/api-band-test$(EXESUF)
fate-api-band: CMD = run $(APITESTSDIR)/api-band-test$(EXESUF) $(TARGET_SAMPLES)/mpeg4/resize_down-up.h263
fate-api-band: CMP = null

FATE_API_SAMPLES_LIBAVFORMAT-$(call DEMDEC, H264, H264, H264_PARSER) += fate-api-h264
fate-api-h264: $(APITESTSDIR)/api-h264-test$(EXESUF)
fate-api-h264: CMD = run $(APITESTSDIR)/api-h264-test$(EXESUF) $(TARGET_SAMPLES)/h264-conformance/SVA_NL2_E.264

FATE_API_SAMPLES_LIBAVFORMAT-$(call DEMDEC, H264, H264) += fate-api-h264-slice
fate-api-h264-slice: $(APITESTSDIR)/api-h264-slice-test$(EXESUF)
fate-api-h264-slice: CMD = run $(APITESTSDIR)/api-h264-slice-test$(EXESUF) 2 $(TARGET_SAMPLES)/h264/crew_cif.nal

# MVC from packets that do not start on an access unit boundary
FATE_API_H264_MVC = fate-api-h264-mvc-split fate-api-h264-mvc-split-mt \
                    fate-api-h264-mvc-unused-subset-sps fate-api-h264-mvc-late-ps \
                    fate-api-h264-mvc-view-change fate-api-h264-mvc-view-change-late-ps-mt \
                    fate-api-h264-mvc-late-ps-get-format
FATE_API_SAMPLES_LIBAVCODEC-$(CONFIG_H264_DECODER) += $(FATE_API_H264_MVC)
$(FATE_API_H264_MVC): $(APITESTSDIR)/api-h264-mvc-test$(EXESUF)
fate-api-h264-mvc-split:    CMD = run $(APITESTSDIR)/api-h264-mvc-test$(EXESUF) 1 $(TARGET_SAMPLES)/h264/mvc-2view.264
fate-api-h264-mvc-split-mt: CMD = run $(APITESTSDIR)/api-h264-mvc-test$(EXESUF) 2 $(TARGET_SAMPLES)/h264/mvc-2view.264
fate-api-h264-mvc-split-mt: REF = $(SRC_PATH)/tests/ref/fate/api-h264-mvc-split
# a subset SPS with the id of the ordinary SPS but other views, which nothing
# refers to and which must not hide the dependent view
fate-api-h264-mvc-unused-subset-sps: CMD = run $(APITESTSDIR)/api-h264-mvc-test$(EXESUF) 1 $(TARGET_SAMPLES)/h264/mvc-2view.264 prefix=0000016f76040cac13122a1419956b5c0215a20000
fate-api-h264-mvc-unused-subset-sps: REF = $(SRC_PATH)/tests/ref/fate/api-h264-mvc-split
# the subset SPS and the dependent view's PPS in the packet of the dependent view
fate-api-h264-mvc-late-ps: CMD = run $(APITESTSDIR)/api-h264-mvc-test$(EXESUF) 1 $(TARGET_SAMPLES)/h264/mvc-2view.264 late_ps
fate-api-h264-mvc-late-ps: REF = $(SRC_PATH)/tests/ref/fate/api-h264-mvc-split
# select views only after the callback can see their availability
fate-api-h264-mvc-late-ps-get-format: CMD = run $(APITESTSDIR)/api-h264-mvc-test$(EXESUF) 1 $(TARGET_SAMPLES)/h264/mvc-2view.264 late_ps get_format
fate-api-h264-mvc-late-ps-get-format: REF = $(SRC_PATH)/tests/ref/fate/api-h264-mvc-split
# followed by a coded video sequence with view_ids 0 and 2, declared by a subset
# SPS and referred to by a PPS of other ids
H264_MVC_VIEW_CHANGE = second=6f76040c6b04c48a8506655ad700854880,684fa40462c0
fate-api-h264-mvc-view-change: CMD = run $(APITESTSDIR)/api-h264-mvc-test$(EXESUF) 1 $(TARGET_SAMPLES)/h264/mvc-2view.264 $(H264_MVC_VIEW_CHANGE)
fate-api-h264-mvc-view-change-late-ps-mt: CMD = run $(APITESTSDIR)/api-h264-mvc-test$(EXESUF) 2 $(TARGET_SAMPLES)/h264/mvc-2view.264 $(H264_MVC_VIEW_CHANGE) late_ps
fate-api-h264-mvc-view-change-late-ps-mt: REF = $(SRC_PATH)/tests/ref/fate/api-h264-mvc-view-change

FATE_API_LIBAVFORMAT-yes += $(if $(findstring fate-lavf-flv,$(FATE_LAVF_CONTAINER)),fate-api-seek)
fate-api-seek: $(APITESTSDIR)/api-seek-test$(EXESUF) fate-lavf-flv
fate-lavf-flv: KEEP_FILES ?= 1
fate-lavf-flv: CMP =
fate-api-seek: CMD = run $(APITESTSDIR)/api-seek-test$(EXESUF) $(TARGET_PATH)/tests/data/lavf/lavf.flv 0 720
fate-api-seek: CMP = null

FATE_API-$(HAVE_THREADS) += fate-api-threadmessage
fate-api-threadmessage: $(APITESTSDIR)/api-threadmessage-test$(EXESUF)
fate-api-threadmessage: CMD = run $(APITESTSDIR)/api-threadmessage-test$(EXESUF) 3 10 30 50 2 20 40
fate-api-threadmessage: CMP = null

FATE_API_SAMPLES-$(CONFIG_AVFORMAT) += $(FATE_API_SAMPLES_LIBAVFORMAT-yes)
FATE_API_SAMPLES-$(CONFIG_AVCODEC)  += $(FATE_API_SAMPLES_LIBAVCODEC-yes)

ifdef SAMPLES
    FATE_API_SAMPLES += $(FATE_API_SAMPLES-yes)
endif

FATE_API-$(CONFIG_AVCODEC) += $(FATE_API_LIBAVCODEC-yes)
FATE_API-$(CONFIG_AVFORMAT) += $(FATE_API_LIBAVFORMAT-yes)
FATE_API = $(FATE_API-yes)

FATE-yes += $(FATE_API) $(FATE_API_SAMPLES)

fate-api: $(FATE_API) $(FATE_API_SAMPLES)
