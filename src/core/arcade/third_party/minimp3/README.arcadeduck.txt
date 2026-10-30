minimp3 MP3 decoder (single header) from https://github.com/lieff/minimp3,
as distributed in MAME's 3rdparty/minimp3/minimp3.h (MAME commit
655bb2ebf60caa4aec56c52a6cc7b46111d118e1).

License: CC0 1.0 (public domain dedication), see the header.

ArcadeDuck change: mp3dec_decode_frame() zero-initializes its scratch buffer, because
lanes of scratch.syn that no band writes were copied into qmf_state, making the decoder
state depend on stack contents (observed as differing qmf_state between identical runs).
