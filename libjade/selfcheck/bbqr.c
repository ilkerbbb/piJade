#include <string.h>

#include "bbqr.h"
#include "qrscan.h"
#include "selfcheck.h"

#include <wally_core.h>

// BBB-AIRGAP: vectors for the BBQr collector (main/bbqr.c).  Every multi-frame transfer below was
// produced by the reference encoder in coinkite/BBQr (python/bbqr/utils.py encode_data) and fed
// back through its own join_qrs() before being written here, so the frames are the reference's own
// output rather than this fork's idea of it.  The payload is test_data/1in2out.psbt from the same
// repository, 675 bytes.
//
// The generator lives outside the repo (it imports the vendored reference, which is audit
// material rather than a build dependency); what it emitted is checked in here verbatim.

typedef struct {
    const char* text;
    size_t len;
} bbqr_frame_t;

// 675-byte PSBT, the payload of the Z, base32 and hex transfers
static const char PSBT_HEX[]
    = "70736274FF0100C902000000030D0B15EDA761956158644571CB32675916CE29DE46C52ADB133F80902A7C7BD8000000"
      "0000FFFFFFFF4F72B07125366B7AA7F09F68A6F02CEB535B6EBB854E679BA3864280827FB2E90000000000FFFFFFFF77"
      "F5752181606A93589CAF3704005426B2FEA128FC7DB65CEE422DB781BCB9170000000000FFFFFFFF028CCFF008000000"
      "001976A914EC019301C44B9EBB0D9A2488E0F13E9652535F2C88AC8CCFF008000000001976A914D873DAEB3D98EDA8DB"
      "A55A04E97E0FE87E30A59888AC00000000000100550200000001ADDE000000000000EFBE000000000000000000000000"
      "000000000000000000004900000000FFFFFFFF0100E1F505000000001976A9140B2537A7D6F3CC668C9E9FA0303FFB3C"
      "AD6E9B8188AC000000002206032BE372801D8460DDA52AE178AAD774A54800BA56F949C97B8A2F51E299209D060C0F05"
      "69430000000000000000000100550200000001ADDE000000000000EFBE00000000000000000000000000000000000000"
      "0000004900000000FFFFFFFF0100E1F505000000001976A914828B746C9CC6E2FBB89394F54FF40EFFD2A4B65188AC00"
      "000000220602D922903B8EB295BB232FC0304D88CF1755A60179156FA8F84970F0D4072D5FBB0C0F0569430000000001"
      "000000000100550200000001ADDE000000000000EFBE0000000000000000000000000000000000000000000049000000"
      "00FFFFFFFF0100E1F505000000001976A9141188D9744C0FE8E8FAEC772D0C481BF81400F90988AC000000002206021F"
      "B423D42512E49D75A9D27415047A74378243CF7AEE07C1B69BA7DA0A03D4280C0F056943000000000200000000002202"
      "02F2D482365A1E8E9BBFCC830141048E9949F8EFBAE9B9DB6BBFABD85498B5F647100F0569430C000000220000006003"
      "000000";

// First 100 bytes of the same PSBT, the payload of the single-frame transfer
static const char SHORT_HEX[]
    = "70736274FF0100C902000000030D0B15EDA761956158644571CB32675916CE29DE46C52ADB133F80902A7C7BD8000000"
      "0000FFFFFFFF4F72B07125366B7AA7F09F68A6F02CEB535B6EBB854E679BA3864280827FB2E90000000000FFFFFFFF77"
      "F5752181";

// 40 runs of 5 identical bytes, the payload of the base36 transfer
static const char SYNTH_HEX[]
    = "000000000001010101010202020202030303030304040404040505050505060606060607070707070808080808090909"
      "09090A0A0A0A0A0B0B0B0B0B0C0C0C0C0C0D0D0D0D0D0E0E0E0E0E0F0F0F0F0F10101010101111111111121212121213"
      "131313131414141414151515151516161616161717171717181818181819191919191A1A1A1A1A1B1B1B1B1B1C1C1C1C"
      "1C1D1D1D1D1D1E1E1E1E1E1F1F1F1F1F2020202020212121212122222222222323232323242424242425252525252626"
      "2626262727272727";

// Z: raw deflate then base32, three parts
static const bbqr_frame_t Z_FRAMES[] = {
    { "B$"
      "ZP0300FMUE4KXZZ7EHBEUJQGAYDGMXLP2O34WEVGERCKNOQWTY3URDYXHGTXTTHOVHKW6YXZQYEVSN6UGQMEHYB4CP4RI3BJK43MVLSZ7ZRH5R5S"
      "B4527A5C6N3LL65GZRPNZZGU2NK332BFLVR7VVKSYTCILLOLCJZ5PGFQGCC2U37YW5J6CTXMW6NHMT533MMPJ3YXQQVGL2ZZ76AADRETFVNCV4MG"
      "OMY6GEPPPG5XSZFIOQ6PW2JUFQVDWX",
        256 },
    { "B$"
      "ZP03015FMIHLHCI3Y23V5WGPPK5OF5GSFOKZI576FTVA5FGM5NNACNMNSAQBLZQZY62PJQS7Q72PQGVTABGZRVEPB4HL5MGCB3SVONS5P7W7BGVV"
      "T557AFA33L63OW43GW5BAYVTCMNLH5XCUEDNRF4HXFFLMHCWV25FZM6VQNQFPW2PZWI5MXP3QKHGIKOPMXR6CZGOO6CFSQ42IKN3USTQ44OHX53Y"
      "YXTSSX74XXZ7ZPFXMRMCDTBDJU3JIJ",
        256 },
    { "B$"
      "ZP03022Z6ZXJXOK3LD6YHA3NYV4PDUDFR2K2H6RIPZ4BI7V2YOXRXPQY5YJELSQ4EHNXBM6HQX74PC26NXEXI6B7UR6IQMH446CDUR36RHYRKV5D"
      "E5ZUUVS5FEIWNKJLGJXHGPK66WGP5YNX3PEW24ZRLTJYAOMEBNXIGEYT2OSSUTLGKFZX7M7VTZUGI5LH5GM6X6PC76XZOO3PM7WV67BCM3D5M3XM"
      "AEQBYPJAWRAJZQAMEQA",
        245 },
};

// 2: base32 only, three parts, the last one short
static const bbqr_frame_t B32_FRAMES[] = {
    { "B$"
      "2P0300OBZWE5H7AEAMSAQAAAAAGDILCXW2OYMVMFMGIRLRZMZGOWIWZYU54RWFFLNRGP4ASAVHY66YAAAAAAAA7777772POKYHCJJWNN5KP4E7NC"
      "TPALHLKNNW5O4FJZTZXI4GIKAIE75S5EAAAAAAAD777777O72XKIMBMBVJGWE4V43QIACUE2ZP5IJI7R63MXHOIIW3PAN4XELQAAAAAAAP777774"
      "BIZT7QBAAAAAAADF3KSFHMAGJQDRCLT25Q3GRERDQPCPUWKJJV6LEIVSGM74AIAAAAAAAZO2URJWDT3LVT3GHNVDN2KWQE5F7A72D6GCSZRCFMAA"
      "AAAAAAAEAFKAQAAAAADLO6AAAAAAAAADX34AAA",
        376 },
    { "B$"
      "2P0301AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAJEAAAAAA777777YBADQ7KBIAAAAAAGLWVEKAWJJXU7LPHTDGRSPJ7IBQH75TZLLOTOAYRLAAAA"
      "AAAIQGAMV6G4UADWCGBXNFFLQXRKWXOSSUQAF2K34UTSL3RIXVDYUZECOQMDAPAVUUGAAAAAAAAAAAAAAACACVAIAAAAABVXPAAAAAAAAAB356AA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAESAAAAAAP777774AQBYPVAUAAAAAADF3KSFECRN2GZHGG4L53RE4U6VH7IDX72KSLMUMIVQAAAAAAEI"
      "DAFWJCSA5Y5MUVXMRS7QBQJWEM6F2VUYAXSFLP",
        376 },
    { "B$"
      "2P0302VD4ES4HQ2QDS2X53BQHQK2KDAAAAAAABAAAAAAABABKQEAAAAAA23XQAAAAAAAAA567AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACJAA"
      "AAAAH777776AIA4H2QKAAAAAABS5VJCQIYRWLUJQH6R2H25R3S2DCIDP4BIAHZBGEKYAAAAAACEBQCD62CHVBFCLSJ25NJ2J2BKBD2OQ3YEQ6PPL"
      "XAPQNWTOT5UCQD2QUAYDYFNFBQAAAAAABAAAAAAAACEAQC6LKIENS2D2HJXP6MQMAUCBEOTFE7R3525G45W257VPMFJGFV6ZDRADYFNFBQYAAAAA"
      "RAAAAAMABQAAAA",
        352 },
};

// H: hex, two parts.  The first frame is 1024 characters, which is one MORE than the camera path
// can deliver: the scanner rejects a payload of QR_MAX_PAYLOAD_LENGTH or longer (main/qrscan.c:138
// tests >=, keeping the last byte of its buffer for the terminator), so the largest frame it hands
// over is 1023 characters, and for hex the largest valid one is 1022, the payload having to be an
// even number of characters.  The vector is left at 1024 deliberately: what it measures is the
// collector's own bound on a full-length frame, not the camera boundary, and no camera-path claim
// is made for it.
static const bbqr_frame_t HEX_FRAMES[] = {
    { "B$"
      "HP020070736274FF0100C902000000030D0B15EDA761956158644571CB32675916CE29DE46C52ADB133F80902A7C7BD80000000000FFFFFF"
      "FF4F72B07125366B7AA7F09F68A6F02CEB535B6EBB854E679BA3864280827FB2E90000000000FFFFFFFF77F5752181606A93589CAF370400"
      "5426B2FEA128FC7DB65CEE422DB781BCB9170000000000FFFFFFFF028CCFF008000000001976A914EC019301C44B9EBB0D9A2488E0F13E96"
      "52535F2C88AC8CCFF008000000001976A914D873DAEB3D98EDA8DBA55A04E97E0FE87E30A59888AC00000000000100550200000001ADDE00"
      "0000000000EFBE000000000000000000000000000000000000000000004900000000FFFFFFFF0100E1F505000000001976A9140B2537A7D6"
      "F3CC668C9E9FA0303FFB3CAD6E9B8188AC000000002206032BE372801D8460DDA52AE178AAD774A54800BA56F949C97B8A2F51E299209D06"
      "0C0F0569430000000000000000000100550200000001ADDE000000000000EFBE000000000000000000000000000000000000000000004900"
      "000000FFFFFFFF0100E1F505000000001976A914828B746C9CC6E2FBB89394F54FF40EFFD2A4B65188AC00000000220602D922903B8EB295"
      "BB232FC0304D88CF1755A60179156FA8F84970F0D4072D5FBB0C0F0569430000000001000000000100550200000001ADDE000000000000EF"
      "BE000000000000",
        1024 },
    { "B$"
      "HP0201000000000000000000000000000000004900000000FFFFFFFF0100E1F505000000001976A9141188D9744C0FE8E8FAEC772D0C481B"
      "F81400F90988AC000000002206021FB423D42512E49D75A9D27415047A74378243CF7AEE07C1B69BA7DA0A03D4280C0F0569430000000002"
      "0000000000220202F2D482365A1E8E9BBFCC830141048E9949F8EFBAE9B9DB6BBFABD85498B5F647100F0569430C00000022000000600300"
      "0000",
        342 },
};

// A transfer that fits in one frame
static const bbqr_frame_t SINGLE_FRAME[] = {
    { "B$"
      "2P0100OBZWE5H7AEAMSAQAAAAAGDILCXW2OYMVMFMGIRLRZMZGOWIWZYU54RWFFLNRGP4ASAVHY66YAAAAAAAA7777772POKYHCJJWNN5KP4E7NC"
      "TPALHLKNNW5O4FJZTZXI4GIKAIE75S5EAAAAAAAD777777O72XKIMB",
        168 },
};

// 40 parts, so the total field reads "14".  In base36 that is 40 and in hex it is 20: a reader
// that believes the reference's split.py comment about hex digits finishes at half the data.
static const bbqr_frame_t BASE36_FRAMES[] = {
    { "B$2P1400AAAAAAAA", 16 },
    { "B$2P1401AEAQCAIB", 16 },
    { "B$2P1402AIBAEAQC", 16 },
    { "B$2P1403AMBQGAYD", 16 },
    { "B$2P1404AQCAIBAE", 16 },
    { "B$2P1405AUCQKBIF", 16 },
    { "B$2P1406AYDAMBQG", 16 },
    { "B$2P1407A4DQOBYH", 16 },
    { "B$2P1408BAEAQCAI", 16 },
    { "B$2P1409BEEQSCIJ", 16 },
    { "B$2P140ABIFAUCQK", 16 },
    { "B$2P140BBMFQWCYL", 16 },
    { "B$2P140CBQGAYDAM", 16 },
    { "B$2P140DBUGQ2DIN", 16 },
    { "B$2P140EBYHA4DQO", 16 },
    { "B$2P140FB4HQ6DYP", 16 },
    { "B$2P140GCAIBAEAQ", 16 },
    { "B$2P140HCEIRCEIR", 16 },
    { "B$2P140ICIJBEEQS", 16 },
    { "B$2P140JCMJRGEYT", 16 },
    { "B$2P140KCQKBIFAU", 16 },
    { "B$2P140LCUKRKFIV", 16 },
    { "B$2P140MCYLBMFQW", 16 },
    { "B$2P140NC4LROFYX", 16 },
    { "B$2P140ODAMBQGAY", 16 },
    { "B$2P140PDEMRSGIZ", 16 },
    { "B$2P140QDINBUGQ2", 16 },
    { "B$2P140RDMNRWGY3", 16 },
    { "B$2P140SDQOBYHA4", 16 },
    { "B$2P140TDUOR2HI5", 16 },
    { "B$2P140UDYPB4HQ6", 16 },
    { "B$2P140VD4PR6HY7", 16 },
    { "B$2P140WEAQCAIBA", 16 },
    { "B$2P140XEEQSCIJB", 16 },
    { "B$2P140YEIRCEIRC", 16 },
    { "B$2P140ZEMRSGIZD", 16 },
    { "B$2P1410EQSCIJBE", 16 },
    { "B$2P1411EUSSKJJF", 16 },
    { "B$2P1412EYTCMJRG", 16 },
    { "B$2P1413E4TSOJZH", 16 },
};

// Z framing over bytes that are not a deflate stream: collection completes, decompression must not
static const bbqr_frame_t BAD_DEFLATE_FRAMES[] = {
    { "B$"
      "ZP0100OBZWE5H7AEAMSAQAAAAAGDILCXW2OYMVMFMGIRLRZMZGOWIWZYU54RWFFLNRGP4ASAVHY66YAAAAAAAA7777772POKYHCJJWNN5KP4E7NC"
      "TPALHLKNNW5O4FJZTZXI4GIKAIE75S5EAAAAAAAD777777O72XKIMB",
        168 },
};

// A valid deflate stream of 819200 zero bytes, which is over MAX_INPUT_MSG_SIZE.  Two small
// frames, and what they inflate to is what has to be refused.
static const bbqr_frame_t BOMB_FRAMES[] = {
    { "B$"
      "ZP02005XATCAIAAAAMFIHVJ5WQM75AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAA",
        1024 },
    { "B$"
      "ZP0201AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAGXAA",
        290 },
};

// Same header and index as B32_FRAMES[0], one payload character different
static const bbqr_frame_t CONFLICTING_FRAME
    = { "B$"
        "2P0300OBZWE5H7AEAMSAQAAAAAGDILCXW2OYMVMFMGIRLRZMZGOWIWZYU54RWFFLNRGP4ASAVHY66YAAAAAAAA7777772POKYHCJJWNN5KP4E7"
        "NCTPALHLKNNW5O4FJZTZXI4GIKAIE75S5EAAAAAAAD777777O72XKIMBMBVJGWE4V43QIACUE2ZP5IJI7R63MXHOIIW3PAN4XELQAAAAAAAP77"
        "7774BIZT7QBAAAAAAADF3KSFHMAGJQDRCLT25Q3GRERDQPCPUWKJJV6LEIVSGM74AIAAAAAAAZO2URJWDT3LVT3GHNVDN2KWQE5F7A72D6GCSZ"
        "RCFMAAAAAAAAAEAFKAQAAAAADLO6AAAAAAAAADX34AAQ",
          376 };

// Non-final part whose payload is 4 characters short of a multiple of 8
static const bbqr_frame_t MISALIGNED_FRAME
    = { "B$"
        "2P0300OBZWE5H7AEAMSAQAAAAAGDILCXW2OYMVMFMGIRLRZMZGOWIWZYU54RWFFLNRGP4ASAVHY66YAAAAAAAA7777772POKYHCJJWNN5KP4E7"
        "NCTPALHLKNNW5O4FJZTZXI4GIKAIE75S5EAAAAAAAD777777O72XKIMBMBVJGWE4V43QIACUE2ZP5IJI7R63MXHOIIW3PAN4XELQAAAAAAAP77"
        "7774BIZT7QBAAAAAAADF3KSFHMAGJQDRCLT25Q3GRERDQPCPUWKJJV6LEIVSGM74AIAAAAAAAZO2URJWDT3LVT3GHNVDN2KWQE5F7A72D6GCSZ"
        "RCFMAAAAAAAAAEAFKAQAAAAADLO6AAAAAAAAADX3",
          372 };

// B32_FRAMES[1] with the file type changed from 'P' to 'T'.  Same encoding, same part count, same
// index, same length: only the file type says it belongs to another transfer.
static const bbqr_frame_t FOREIGN_FRAME
    = { "B$"
        "2T0301AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAJEAAAAAA777777YBADQ7KBIAAAAAAGLWVEKAWJJXU7LPHTDGRSPJ7IBQH75TZLLOTOAYRLAA"
        "AAAAAIQGAMV6G4UADWCGBXNFFLQXRKWXOSSUQAF2K34UTSL3RIXVDYUZECOQMDAPAVUUGAAAAAAAAAAAAAAACACVAIAAAAABVXPAAAAAAAAAB3"
        "56AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAESAAAAAAP777774AQBYPVAUAAAAAADF3KSFECRN2GZHGG4L53RE4U6VH7IDX72KSLMUMIVQAA"
        "AAAAEIDAFWJCSA5Y5MUVXMRS7QBQJWEM6F2VUYAXSFLP",
          376 };

// B32_FRAMES[1] with 8 characters removed: still 8-aligned, so only the block length catches it
static const bbqr_frame_t SHORT_PART_FRAME
    = { "B$"
        "2P0301AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAJEAAAAAA777777YBADQ7KBIAAAAAAGLWVEKAWJJXU7LPHTDGRSPJ7IBQH75TZLLOTOAYRLAA"
        "AAAAAIQGAMV6G4UADWCGBXNFFLQXRKWXOSSUQAF2K34UTSL3RIXVDYUZECOQMDAPAVUUGAAAAAAAAAAAAAAACACVAIAAAAABVXPAAAAAAAAAB3"
        "56AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAESAAAAAAP777774AQBYPVAUAAAAAADF3KSFECRN2GZHGG4L53RE4U6VH7IDX72KSLMUMIVQAA"
        "AAAAEIDAFWJCSA5Y5MUVXMRS7QBQJWEM6F2V",
          368 };

// Headers that must be turned away without reaching the decoders
static const bbqr_frame_t BAD_FRAMES[] = {
    { "B$2P0200", 8 }, // header only, no payload at all
    { "B$XP0100AAAAAAAA", 16 }, // encoding is not H, 2 or Z
    { "B$2?0100AAAAAAAA", 16 }, // file type outside the standard's set
    { "B$2P0000AAAAAAAA", 16 }, // zero parts
    { "B$2P0102AAAAAAAA", 16 }, // index past the last part
    { "B$2P0a00AAAAAAAA", 16 }, // lowercase base36 digit, which alphanumeric QR cannot carry
    { "B$2P01", 6 }, // truncated header
    { "C$2P0100AAAAAAAA", 16 }, // wrong magic
    { "B$2P0100AAAAAAA!", 16 }, // character outside the base32 alphabet
    { "B$HP0100ABC", 11 }, // hex payload with an odd number of characters
    { "B$HP0100ZZ", 10 }, // even-length hex payload outside the hex alphabet
    { "B$2P0100AAA", 11 }, // base32 length that cannot complete a byte (3 characters, 15 bits)
    { "B$2P0100AA======", 16 }, // base32 padding, which BBQr strips and base32_to_bin() stops at
    { "B$2P0100AA\0BBBBB", 16 }, // embedded NUL, which base32_to_bin() also stops at
};

static bool feed(bbqr_ctx_t* ctx, const bbqr_frame_t* frame, const bbqr_collect_result_t expected)
{
    const bbqr_collect_result_t result = bbqr_collect(ctx, (const uint8_t*)frame->text, frame->len);
    if (result != expected) {
        JADE_LOGE("BBQr frame gave %d, expected %d", result, expected);
        return false;
    }
    return true;
}

// Feed a transfer in order; every frame but the last must report progress
static bool feed_all(bbqr_ctx_t* ctx, const bbqr_frame_t* frames, const size_t num_frames)
{
    for (size_t i = 0; i < num_frames; ++i) {
        if (!feed(ctx, &frames[i], i + 1 == num_frames ? BBQR_COMPLETE : BBQR_IN_PROGRESS)) {
            return false;
        }
    }
    return true;
}

static bool check_payload(bbqr_ctx_t* ctx, const char* expected_hex, const char expected_type)
{
    const uint8_t* data = NULL;
    size_t data_len = 0;
    char file_type = 0;
    if (!bbqr_finalise(ctx, &data, &data_len, &file_type)) {
        JADE_LOGE("BBQr finalise failed");
        return false;
    }

    const size_t expected_len = strlen(expected_hex) / 2;
    if (data_len != expected_len || file_type != expected_type) {
        JADE_LOGE("BBQr payload %u bytes type %c, expected %u type %c", (unsigned)data_len, file_type,
            (unsigned)expected_len, expected_type);
        return false;
    }

    uint8_t* expected = JADE_MALLOC(expected_len);
    size_t written = 0;
    const bool match = wally_hex_to_bytes(expected_hex, expected, expected_len, &written) == WALLY_OK
        && written == expected_len && !memcmp(data, expected, expected_len);
    free(expected);

    if (!match) {
        JADE_LOGE("BBQr payload does not match the expected bytes");
        return false;
    }
    return true;
}

// The final part arriving first is the case that deadlocks a collector which takes the block
// length from whatever frame it sees first: the last part is the short one, so every full part
// after it looks wrong.  A cycling animation puts the camera there once every N transfers.
static bool test_final_part_first(void)
{
    bbqr_ctx_t ctx = { 0 };
    bool ok = feed(&ctx, &Z_FRAMES[2], BBQR_REJECTED) && feed(&ctx, &Z_FRAMES[0], BBQR_IN_PROGRESS)
        && feed(&ctx, &Z_FRAMES[1], BBQR_IN_PROGRESS) && feed(&ctx, &Z_FRAMES[2], BBQR_COMPLETE)
        && check_payload(&ctx, PSBT_HEX, 'P');
    bbqr_free(&ctx);
    return ok;
}

static bool test_transfer(const bbqr_frame_t* frames, const size_t num_frames, const char* expected_hex)
{
    bbqr_ctx_t ctx = { 0 };
    const bool ok = feed_all(&ctx, frames, num_frames) && check_payload(&ctx, expected_hex, 'P');
    bbqr_free(&ctx);
    return ok;
}

// A repeat of a part already held is what an animated code delivers every cycle and must change
// nothing.  The same index carrying different bytes is a different transfer sharing these six
// header characters, and merging the two would build one corrupt buffer, so collection restarts.
static bool test_repeated_parts(void)
{
    bbqr_ctx_t ctx = { 0 };
    bool ok = feed(&ctx, &B32_FRAMES[0], BBQR_IN_PROGRESS) && feed(&ctx, &B32_FRAMES[1], BBQR_IN_PROGRESS);
    if (ok && ctx.num_seen != 2) {
        JADE_LOGE("BBQr held %u parts, expected 2", (unsigned)ctx.num_seen);
        ok = false;
    }

    ok = ok && feed(&ctx, &B32_FRAMES[0], BBQR_IN_PROGRESS);
    if (ok && ctx.num_seen != 2) {
        JADE_LOGE("BBQr repeat changed the count to %u", (unsigned)ctx.num_seen);
        ok = false;
    }

    ok = ok && feed(&ctx, &CONFLICTING_FRAME, BBQR_IN_PROGRESS);
    if (ok && ctx.num_seen != 1) {
        JADE_LOGE("BBQr conflict left %u parts, expected a restart at 1", (unsigned)ctx.num_seen);
        ok = false;
    }

    bbqr_free(&ctx);
    return ok;
}

static bool test_rejected_frames(void)
{
    for (size_t i = 0; i < sizeof(BAD_FRAMES) / sizeof(BAD_FRAMES[0]); ++i) {
        bbqr_ctx_t ctx = { 0 };
        const bool ok = feed(&ctx, &BAD_FRAMES[i], BBQR_REJECTED);
        bbqr_free(&ctx);
        if (!ok) {
            JADE_LOGE("BBQr accepted bad frame %u", (unsigned)i);
            return false;
        }
    }

    // A non-final part that is not a multiple of 8 characters would shift the byte alignment of
    // every part after it, because base32_to_bin() drops the leftover bits rather than erroring.
    bbqr_ctx_t ctx = { 0 };
    const bool ok = feed(&ctx, &MISALIGNED_FRAME, BBQR_REJECTED);
    bbqr_free(&ctx);
    return ok;
}

// Nothing else checks the deflate stream: BBQr carries no digest and raw deflate has no adler32
static bool test_bad_deflate(void)
{
    bbqr_ctx_t ctx = { 0 };
    const uint8_t* data = NULL;
    size_t data_len = 0;
    const bool ok = feed_all(&ctx, BAD_DEFLATE_FRAMES, sizeof(BAD_DEFLATE_FRAMES) / sizeof(BAD_DEFLATE_FRAMES[0]))
        && !bbqr_finalise(&ctx, &data, &data_len, NULL) && !data && !data_len;
    bbqr_free(&ctx);
    return ok;
}

// The inflated size is the only thing standing between a handful of frames and an allocation the
// device does not have.  The guard is the output buffer's own size, so the measurement is whether
// finalise refuses a stream that wants more room than MAX_INPUT_MSG_SIZE.
static bool test_inflate_bomb(void)
{
    bbqr_ctx_t ctx = { 0 };
    const uint8_t* data = NULL;
    size_t data_len = 0;
    const bool ok = feed_all(&ctx, BOMB_FRAMES, sizeof(BOMB_FRAMES) / sizeof(BOMB_FRAMES[0]))
        && !bbqr_finalise(&ctx, &data, &data_len, NULL) && !data && !data_len;
    bbqr_free(&ctx);
    return ok;
}

// The other ceiling, reached without any compression at all: "ZZ" is 1295 parts, and a first part
// carrying a full frame makes the block length 635 bytes, so the transfer claims 822325 bytes.
// The setup has to turn that away before it allocates.  The frame is built here rather than
// written out because its payload fills a whole frame with one repeated character.  The length
// comes from the scanner's own ceiling rather than a copy of it: a smaller QR_MAX_PAYLOAD_LENGTH
// would otherwise leave this test feeding a frame that is no longer the largest one accepted.
#define BBQR_MAX_FRAME_PAYLOAD_LEN (QR_MAX_PAYLOAD_LENGTH - BBQR_HEADER_LEN)

static bool test_part_count_ceiling(void)
{
    const size_t len = BBQR_HEADER_LEN + BBQR_MAX_FRAME_PAYLOAD_LEN;
    char* frame = JADE_MALLOC(len);
    memcpy(frame, "B$2PZZ00", BBQR_HEADER_LEN);
    memset(frame + BBQR_HEADER_LEN, 'A', BBQR_MAX_FRAME_PAYLOAD_LEN);

    bbqr_ctx_t ctx = { 0 };
    const bbqr_frame_t oversized = { frame, len };
    const bool ok = feed(&ctx, &oversized, BBQR_REJECTED);
    bbqr_free(&ctx);
    free(frame);
    return ok;
}

// A frame from a second transfer arriving mid-collection.  Length-matched to the part it claims
// to be, so the block-length rule cannot catch it; the header comparison is the only thing that
// does.  Two animated codes in the camera's view is how this happens in the room.
static bool test_foreign_transfer(void)
{
    bbqr_ctx_t ctx = { 0 };
    bool ok = feed(&ctx, &B32_FRAMES[0], BBQR_IN_PROGRESS) && feed(&ctx, &FOREIGN_FRAME, BBQR_REJECTED);
    if (ok && ctx.num_seen != 1) {
        JADE_LOGE("BBQr foreign frame left %u parts, expected 1", (unsigned)ctx.num_seen);
        ok = false;
    }
    bbqr_free(&ctx);
    return ok;
}

// Unequal part lengths are the other thing the reference join_qrs() never checks: it pads and
// decodes each part on its own, so a short middle part silently shortens the reassembled bytes.
// Here the part is still 8-aligned, so only the established block length turns it away.
static bool test_short_middle_part(void)
{
    bbqr_ctx_t ctx = { 0 };
    bool ok = feed(&ctx, &B32_FRAMES[0], BBQR_IN_PROGRESS) && feed(&ctx, &SHORT_PART_FRAME, BBQR_REJECTED);
    if (ok && ctx.num_seen != 1) {
        JADE_LOGE("BBQr short part left %u parts, expected 1", (unsigned)ctx.num_seen);
        ok = false;
    }
    bbqr_free(&ctx);
    return ok;
}

// Half a transfer must not hand anything back: the caller has no other way to tell the difference
static bool test_incomplete_is_not_finalised(void)
{
    bbqr_ctx_t ctx = { 0 };
    const uint8_t* data = NULL;
    size_t data_len = 0;
    char file_type = 'x';
    bool ok = feed(&ctx, &B32_FRAMES[0], BBQR_IN_PROGRESS) && !bbqr_finalise(&ctx, &data, &data_len, &file_type)
        && !data && !data_len && file_type == 'x';
    bbqr_free(&ctx);

    // And an empty context, which is what a cancelled scan leaves behind
    bbqr_ctx_t empty = { 0 };
    ok = ok && !bbqr_finalise(&empty, &data, &data_len, NULL) && !data && !data_len;
    bbqr_free(&empty);
    return ok;
}

static bool test_header_detection(void)
{
    const char not_bbqr[] = "UR:CRYPTO-PSBT/1-2/LPADAO";
    return bbqr_is_header((const uint8_t*)Z_FRAMES[0].text, Z_FRAMES[0].len)
        && !bbqr_is_header((const uint8_t*)not_bbqr, sizeof(not_bbqr) - 1)
        && !bbqr_is_header((const uint8_t*)"B$2P01", 6);
}

static bool test_free_is_safe(void)
{
    bbqr_ctx_t ctx = { 0 };
    bbqr_free(&ctx);
    bbqr_free(&ctx);
    bbqr_free(NULL);
    return true;
}

bool test_bbqr(void)
{
    JADE_LOGI("Testing BBQr multi-frame QR collection");

    return test_header_detection() && test_final_part_first()
        && test_transfer(B32_FRAMES, sizeof(B32_FRAMES) / sizeof(B32_FRAMES[0]), PSBT_HEX)
        && test_transfer(HEX_FRAMES, sizeof(HEX_FRAMES) / sizeof(HEX_FRAMES[0]), PSBT_HEX)
        && test_transfer(SINGLE_FRAME, sizeof(SINGLE_FRAME) / sizeof(SINGLE_FRAME[0]), SHORT_HEX)
        && test_transfer(BASE36_FRAMES, sizeof(BASE36_FRAMES) / sizeof(BASE36_FRAMES[0]), SYNTH_HEX)
        && test_repeated_parts() && test_rejected_frames() && test_bad_deflate() && test_inflate_bomb()
        && test_part_count_ceiling() && test_foreign_transfer() && test_short_middle_part()
        && test_incomplete_is_not_finalised() && test_free_is_safe();
}

bool debug_selfcheck(jade_process_t* process)
{
    if (!test_bbqr()) {
        FAIL();
    }
    return true;
}
