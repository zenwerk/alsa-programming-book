/******************************************************
 LPCM WAVEフォーマット・ヘッダ
 ヘッダ・ファイル：WaveFormat.h
 ******************************************************/
#define FORMAT_CHUNK_PCM_SIZE (16)		/* 標準LPCM 'fmt 'サブチャンクサイズ */
#define FORMAT_CHUNK_EX_SIZE (18)		/* 非PCM WAVE 'fmt 'サブチャンクサイズ */
#define FORMAT_CHUNK_EXTENSIBLE_SIZE (40) 	/* 拡張WAVE 'fmt 'サブチャンクサイズ */

#define WAVE_FORMAT_PCM 	(0x0001)	/* 標準LPCM フォーマットコード */
#define WAVE_FORMAT_EXTENSIBLE 	(0xfffe) 	/* 拡張WAVE  フォーマットコード */
#define WAVE_GUID_TAG	"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71"

/* WAVEチャンクIDコード */
static char RIFF_ID[4] = {'R', 'I', 'F', 'F'};
static char WAVE_ID[4] = {'W', 'A', 'V', 'E'};
static char FMT_ID[4] = {'f', 'm', 't', ' '};
static char DATA_ID[4] = {'d', 'a', 't', 'a'};

/* WORD型の定義 */
typedef unsigned char BYTE;			/* 8bit符号無し整数型 */
typedef unsigned short WORD;			/* 16bit符号無し整数型 */
typedef unsigned int DWORD;			/* 32bit符号無し整数型 */
typedef DWORD FOURCC;				/* 4文字コードの整数型 */

/* Globaly Unique IDentifier(GUID) */
typedef struct guid{
  WORD	subFormatCode;
  BYTE	wave_guid_tag[14] ;
} GUID;

/* 再生WAVEサウンドフォーマット構造体の定義 */
typedef struct format_descriptor{
  WORD  formatTag;				/* フォーマットコード */
  WORD	numChannels;				/* チャンネル数 */
  DWORD samplesPerSec;				/* 標本化周波数:fs(Hz) */
  DWORD avgBytesPerSec;				/* 転送レート：dataFrameSize * fs (bytes/sec) */
  WORD  dataFrameSize;				/* フレームサイズ：numChannels * bitsPerSample / 8 (bytes) */
  WORD  bitsPerSample;				/* サンプル量子化ビット数 (16, 24) */
}WAVEFORMATDESC;

/* 再生サウンド・ファイル構造体の定義 */
typedef struct waveFile_descriptor{
  int	fd;					/* 再生ファイル記述子 */
  long	frameSize;				/* 再生サウンドフレームサイズ（frames) */
}WAVEFILEDESC;


