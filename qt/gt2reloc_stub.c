// Minimal stub for the gt2reloc CLI build — supplies the stereo / dual-SID
// runtime flags introduced for the Qt build. Defaults match the historical
// mono behaviour, so gt2reloc keeps producing single-SID PRG / SID exports.
//
// The datafile[] symbol (the compiled-in C64 playroutine blob) is NOT here:
// it is generated at build time from src/goattrk2.seq into goatdata.c and
// linked in, so io_openlinkeddatafile() finds player.s / altplayer.s and the
// relocator can actually pack a .sid / .prg. See qt/CMakeLists.txt.
int stereo_mode = 0;
unsigned sid2model = 0;
