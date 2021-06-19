/*****************************************************************************
 実例プログラム：FLACサウンド・ファイル再生プログラム
 		     -  標準read/write転送  -
 ソースコード：flac_rw_player_int.c
 ****************************************************************************/
#include <stdbool.h>
#include <getopt.h>
#include "alsa/asoundlib.h"
#include "FLAC/stream_decoder.h" 
#include "FLAC/metadata.h"

/* ユーティリティ関数のプロトタイプ宣言 */
static int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *hwparams);
static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams);
static long flac_read_int_frames (int *datablock, long nFrames);
static void buffer2block(void);
static int flac_write_int(snd_pcm_t *handle);
static void usage(void);
static snd_pcm_sframes_t (*writei_func)(snd_pcm_t *handle, const void *buffer, snd_pcm_uframes_t size);

/*** ALSAライブラリのパラメータ初期化 ***/
static char *device = "plughw:0,0";			/* 再生PCMデバイス名*/
static snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;	/* サンプル・コンテナのフォーマット */
static unsigned int rate = 44100;			/* 標本化速度(Hz) */
static unsigned int numChannels = 1;			/* チャンネル数 */
static unsigned int buffer_time = 0;			/* バッファ時間長(μsec) */
static unsigned int period_time = 0;			/* 転送周期時間長(μsec) */
static snd_pcm_uframes_t buffer_size = 0;		/* バッファサイズ(符号無しフレーム数) */
static snd_pcm_uframes_t period_size = 0;		/* データブロック・サイズ(符号無しフレーム数) */
static snd_output_t *output = NULL;			/* 出力オブジェクトに 対するALSA内部構造体へのハンドル */

/*** アプリケーション制御フラグの初期化 ***/
static int mmap = 0;					/* 転送方法制御フラグ: write=0, mmap write=1  */
static int verbose = 0;					/* 詳細情報表示フラグ: set=1 clear=0 */
static int resample = 1;				/* 標本化速度変換設定フラグ: set=1 clear=0 */

/* libFLAC規定のコールバック関数の宣言 */
static FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, 
							const FLAC__int32 * const buffer[], void *user_data);
static void metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *user_data);
static void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *user_data);

/*** 共通ユーザデータの定義、宣言 ***/
typedef struct{
  FLAC__StreamDecoder *decoder;				/* FLACデコーダ */
  FLAC__uint64 total_frames;				/* 総サンプルフレーム数 */
  const FLAC__Frame *frame;				/* FLAC frame 構造体 */
  const FLAC__int32 *const *dec_buffer;			/* デコーダバッファ */  
  int buffer_pos;					/* デコーダバッファの現在位置  */
  int size_spec;					/* ブロックサイズ仕様適合性識別フラグ: 適合=1 不適合=0 */
  void* dataBlock;					/* 転送データブロック */
  int block_pos;					/* 転送データブロックの現在位置 */
  long counter;						/* 要求に対して、未転送のサンプル・フレーム数 */
  unsigned int qbits;					/* 量子化ビット数 */
} FLAC_DECODER ;

static FLAC_DECODER dflac;

/* PCMにHWパラメータを設定するユーティリティ関数の定義 */
int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *hwparams)
{
  unsigned int rateNear;
  int err, dir;

  /* PCMに対する全構成空間のパラメータを充填する */
  err = snd_pcm_hw_params_any(handle, hwparams);
  if (err < 0) {
    fprintf(stderr, "ハードウェア構成破綻: 適用できるハードウェア構成が無い: %s\n", snd_strerror(err));
    return err;
  }
  /* 構成空間を実際のハードウェア標本化速度のみを包含するように制限する */
  err = snd_pcm_hw_params_set_rate_resample(handle, hwparams, resample);
  if (err < 0) {
    fprintf(stderr, "再標本化の設定失敗: %s\n", snd_strerror(err));
    return err;
  }
  /* 構成空間を実際のアクセス方法のみを包含するように制限する */
  if (mmap) {
    err = snd_pcm_hw_params_set_access(handle, hwparams,
				       SND_PCM_ACCESS_MMAP_INTERLEAVED);
  } else
    err = snd_pcm_hw_params_set_access(handle, hwparams,
				       SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0) {
    fprintf(stderr, "アクセスタイプ非適用: %s\n", snd_strerror(err));
    return err;
  }
	
  /* 構成空間を唯一のフォーマットを包含するように制限する */
  err = snd_pcm_hw_params_set_format(handle, hwparams, format);
  if (err < 0) {
    fprintf(stderr, "サンプルフォーマット非適用: %s\n", snd_strerror(err));
    fprintf(stderr, "適用可能フォーマット:\n");
    for (int fmt = 0; fmt <= SND_PCM_FORMAT_LAST; fmt++) {
      if (snd_pcm_hw_params_test_format(handle, hwparams, (snd_pcm_format_t)fmt) == 0)
	fprintf(stderr, "- %s\n", snd_pcm_format_name((snd_pcm_format_t)fmt));
    }
    return err;
  }
	
  /* 構成空間を唯一のチャンネル数を包含するように制限する */
  err = snd_pcm_hw_params_set_channels(handle, hwparams, numChannels);
  if (err < 0) {
    fprintf(stderr, "チャンネル数 (%i) は非適用: %s\n", numChannels, snd_strerror(err));
    return err;
  }
  /* 構成空間を標本化速度要求値に最も近い値に制限する */
  rateNear = rate;
  err = snd_pcm_hw_params_set_rate_near(handle, hwparams, &rateNear, 0);
  if (err < 0) {
    fprintf(stderr, "標本化速度 %iHz は非適用: %s\n", rate, snd_strerror(err));
    return err;
  }
  if (rateNear != rate) {
    fprintf(stderr, "標本化速度が整合しない (要求値 %iHz, 取得値 %iHz)\n", rate, rateNear);
    return -EINVAL;
  }

  /* 構成空間からbuffer_timeおよびperiod_timeの最大値を抽出する */
  err = snd_pcm_hw_params_get_buffer_time_max(hwparams, &buffer_time, &dir); 
  if (buffer_time > 500000)
    buffer_time = 500000; /* buffer timeの上限を500 msecに設定 */
  if (buffer_time > 0)
    period_time = buffer_time / 4; /* bufferを4つのperiods(チャンク)に分割 */
  else{
    fprintf(stderr, "エラー: buffer_timeはゼロまたは負の値\n");
    return -EINVAL;
  }
		
  /* 構成空間をbuffer_time要求値に最も近い値に制限する */
  err = snd_pcm_hw_params_set_buffer_time_near(handle, hwparams, &buffer_time, &dir);
  if (err < 0) {
    fprintf(stderr, "buffer time設定不可 %i : %s\n", buffer_time, snd_strerror(err));
    return err;
  }
	
  /* 構成空間をperiod_time要求値に最も近い値に制限する */
  err = snd_pcm_hw_params_set_period_time_near(handle, hwparams, &period_time, &dir);
  if (err < 0) {
    fprintf(stderr, "period time設定不可 %i : %s\n", period_time, snd_strerror(err));
    return err;
  }
        
  /* 構成空間から選定された唯一のPCM ハードウェア構成を導入し、PCMの準備を行う */
  err = snd_pcm_hw_params(handle, hwparams);
  if (err < 0) {
    fprintf(stderr, "ハードウェアパラメータ設定不可: %s\n", snd_strerror(err));
    return err;
  }
	
  /* 構成空間からbuffer_sizeとperiod_sizeを取得する */
  err = snd_pcm_hw_params_get_buffer_size(hwparams, &buffer_size);
  if (err < 0) {
    fprintf(stderr, "buffer size取得不可 : %s\n", snd_strerror(err));
    return err;
  }
  err = snd_pcm_hw_params_get_period_size(hwparams, &period_size, &dir);
  if (err < 0) {
    fprintf(stderr, "period size取得不可 : %s\n", snd_strerror(err));
    return err;
  }
  return 0;
}

/* PCMにSWパラメータを設定するユーティリティ関数の定義 */
int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams)
{
  int err;

  /* PCMに対する現在のソフトウェア構成を戻す */
  err = snd_pcm_sw_params_current(handle, swparams);
  if (err < 0) {
    fprintf(stderr, "現在のソフトウェアパラメータ確定不可: %s\n", snd_strerror(err));
    return err;
  }
	
  /* バッファが殆ど満杯となる再生開始閾値(frames)を設定する */
  err = snd_pcm_sw_params_set_start_threshold(handle, swparams, (buffer_size / period_size) * period_size);
  if (err < 0) {
    fprintf(stderr, "再生開始閾値モード設定不可: %s\n", snd_strerror(err));
    return err;
  }
        
  /* 少なくともperiod_size分のサンプルが処理可能な時に再生を許可する */
  err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
  if (err < 0) {
    fprintf(stderr, "avail min設定不可: %s\n", snd_strerror(err));
    return err;
  }
	
  /* ソフトウェアパラメータを再生デバイスに書き込む */
  err = snd_pcm_sw_params(handle, swparams);
	
  if (err < 0) {
    fprintf(stderr, "ソフトウェアパラメータ設定不可: %s\n", snd_strerror(err));
    return err;
  }
  return 0;
}
 
/* 要求サンプル・フレーム数のFLACデータのデコードを制御するユーティリティ関数の定義 */
long flac_read_int_frames(int *dataBlock, long nFrames)
{	
  dflac.block_pos = 0;
  dflac.dataBlock = dataBlock;
  dflac.counter = nFrames;
  dflac.size_spec = 1;

  /* デコーダバッファ内に未転送データがあれば、データブロックに転送する */	
  if (dflac.frame != NULL && dflac.buffer_pos < dflac.frame->header.blocksize)
    buffer2block();

  /* FLACフレームをデコードする */
  while (dflac.block_pos < (int)nFrames * (int)numChannels){
    if (FLAC__stream_decoder_process_single(dflac.decoder) == 0){
      fprintf(stderr, "デコードエラー\n");
      return -1;
    }
    if (dflac.size_spec == 0){
      return -2;
    }
    if (FLAC__stream_decoder_get_state(dflac.decoder) >= FLAC__STREAM_DECODER_END_OF_STREAM)
      break;
  } ;
  dflac.dataBlock = NULL;
  return (long)(dflac.block_pos/(int)numChannels);
}

/* デコーダバッファのデータをデータブロックに転送するユーティリティ関数の定義 */
void buffer2block(void)
{	
  const FLAC__Frame *frame = dflac.frame;
  const FLAC__int32 *const *dec_buffer = dflac.dec_buffer;

  int *dataBlock = (int *)dflac.dataBlock;
  unsigned int shift = 32 - frame->header.bits_per_sample;
  for (int i = 0 ; i < (int)frame->header.blocksize && dflac.counter > 0 ; i++){	
    if (dflac.buffer_pos >= (int)frame->header.blocksize)
      break ;
    for (int j = 0 ; j < (int)frame->header.channels ; j++)
      dataBlock [dflac.block_pos + j] = dec_buffer [j][dflac.buffer_pos] << shift;
    dflac.block_pos += (int)frame->header.channels;
    dflac.counter --;
    dflac.buffer_pos++;
  }				
  return;
}

/* FLACサウンドデータの読込み、および再生を制御するユーティリティ関数の定義(標準read/write転送) */
int flac_write_int(snd_pcm_t *handle)
{
  int *bufPtr;						/* 再生フレームバッファ */
  const long numSoundFrames = (long)dflac.total_frames;	/* 再生サウンド総フレーム数 */
  long nFrames, frameCount, numPlayFrames = 0;		/* 再生済フレーム数の初期化 */
  long readFrames, resFrames = numSoundFrames;		/* 未再生フレーム数の初期化 */
  int err = 0; 
	
  /*  オーディオサンプルの転送に適用するデータブロックにメモリを割り当てる  */	
  int *frameBlock = (int *)malloc(period_size * sizeof(int) * numChannels);
  if (frameBlock == NULL) {
    fprintf(stderr, "メモリ不足でデータブロックを割当てられない\n");
    err = EXIT_FAILURE;
    goto cleaning;
  }
  nFrames = (long)period_size;	/* サウンドファイルから読み込むフレーム数の初期化 */
  while(resFrames>0){
    readFrames = flac_read_int_frames (frameBlock, nFrames);
    if (readFrames < 0) {
      fprintf(stderr, "エラーによりFLACデコード中止\n");
      err = EXIT_FAILURE;
      goto cleaning;
    }
    frameCount = readFrames;	/* 書き込むサンプルフレーム数の初期値をサウンドファイルから読み込むフレーム数に設定 */
    bufPtr = frameBlock;	/* 書き込むサンプルのポインタの初期値をフレームブロックの先頭に設定 */
    while (frameCount > 0) {
      err = (int)writei_func(handle, bufPtr, (snd_pcm_uframes_t)frameCount); /* PCMデバイスにサウンドフレームを転送 */
      if (err == -EAGAIN)
	continue;
      if (err < 0) {
	if (snd_pcm_recover(handle, err, 0) < 0) {
	  fprintf(stderr, "Write転送エラー: %s\n", snd_strerror(err));
	  goto cleaning;
	}
	break;				/* １データブロック周期をスキップ */
      } 
      bufPtr += err *numChannels;	/* フレームバッファのポインタを実際に書いたフレーム数にチャンネル数を乗じた分だけ
					   進める */
      frameCount -= err;		/* フレームバッファ中に残存する書き込み可能なフレーム数を算定 */
    }
    numPlayFrames += readFrames;
		
    /* データ・ブロック長以下の残データフレーム数の計算 */
    if ((resFrames = numSoundFrames - numPlayFrames) <= (long)period_size) 
      nFrames = resFrames;
  }
  snd_pcm_drop(handle);
  printf(" 合計　%lu フレームを再生して終了\n", numPlayFrames);
  err = 0;
 cleaning:
  if(frameBlock != NULL)
    free(frameBlock);
  return err;
}

/* デコードデータを取得するコールバック関数 */
FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,	       
						const FLAC__int32 *const buffer[], void *user_data)
{
  dflac.frame = frame;
  dflac.dec_buffer = buffer;
  dflac.buffer_pos = 0;

  if (frame->header.blocksize > FLAC__MAX_BLOCK_SIZE){	
    fprintf(stderr, "FLACブロックサイズが上限を超えた (%d) > FLAC__MAX_BLOCK_SIZE (%d)\n", frame->header.blocksize, 
	    FLAC__MAX_BLOCK_SIZE);
    dflac.size_spec = 0;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  buffer2block() ;
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/* メタデータを取得するコールバック関数 */
void metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *user_data)
{
  /* FLAC音源パラメータの表示 */
  if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    dflac.total_frames = metadata->data.stream_info.total_samples;
    rate = metadata->data.stream_info.sample_rate;
    numChannels = metadata->data.stream_info.channels;
    dflac.qbits = metadata->data.stream_info.bits_per_sample;

    printf("標本化速度          : %u Hz\n", rate);		
    printf("チャンネル数        : %u\n", numChannels);		
    printf("量子化ビット数      : %u\n", dflac.qbits);		
    printf("総フレーム数        : %ld\n", (long)dflac.total_frames);
  }
  return;
}

/* デコーダのエラーを検出するコールバック関数 */
void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *user_data)
{
  fprintf(stderr, "デコードエラーを検出: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
  return;
}

/* 使用法を表示するユーティリティ関数の定義 */
void usage(void)
{
  int k;
  printf(
	 "使用法: flac_rw_player_int [オプション]... [サウンドファイル]...\n"
	 "-h,--help	  使用法\n"
	 "-D,--device	  再生デバイス\n"
	 "-m,--mmap	  mmap_write転送\n"
	 "-v,--verbose      パラメータ設定値表示\n"
	 "-n,--noresample   再標本化禁止\n"
	 "\n");
  printf("適用サンプルフォーマット:");
  for (k = 0; k < SND_PCM_FORMAT_LAST; ++k) {
    const char *s = snd_pcm_format_name((snd_pcm_format_t)k);
    if (s)
      printf(" %s", s);
  }
  printf("\n");
}

int main(int argc, char *argv[])
{
  static const struct option long_option[] =
    {
      {"help", 0, NULL, 'h'},
      {"device", 1, NULL, 'D'},
      {"mmap", 0, NULL, 'm'},
      {"verbose", 0, NULL, 'v'},
      {"noresample", 0, NULL, 'n'},
      {NULL, 0, NULL, 0},
    };
	
  snd_pcm_t *handle = NULL;		/* PCMハンドル */
  snd_pcm_hw_params_t *hwparams;	/* PCMハードウェア構成空間コンテナ */
  snd_pcm_sw_params_t *swparams;	/* PCMソフトウェア構成コンテナ */
  unsigned char *transfer_method ;	/* 転送方法名 */
  double playtime = 0;			/* 再生時間 */
  int err, c, exit_code = 0;
	
  while ((c = getopt_long(argc, argv, "hD:mvn", long_option, NULL)) != -1) {
    switch (c) {
    case 'h':
      usage();
      return 0;
    case 'D':
      device = strdup(optarg); /* 再生デバイス名の指定 */
      break;
    case 'm':
      mmap = 1;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'n':
      resample = 0;
      break;	
    default:
      fprintf(stderr, "`--help'で使用方法を確認\n");
      return EXIT_FAILURE; 
    }
  }
	
  if (optind > argc-1) {
    usage();
    return 0;
  }
    	
  /* 再生FLACファイル関連変数を初期化する */
  const char *filePath = NULL;
 
  FLAC__bool success = true;
  FLAC__StreamDecoderInitStatus init_status; 
	
  /* ALSA HW, SWパラメータ・コンテナの初期化 */
  snd_pcm_hw_params_alloca(&hwparams); 
  snd_pcm_sw_params_alloca(&swparams);
    	
  /* 再生ファイルパス名を取得する */
  filePath = argv[optind];  
  printf("*** サウンドファイル情報 ***\n");
  printf("ファイル名：%s\n", filePath);
    	
  /* デコーダのインスタンスを生成する */
  if((dflac.decoder = FLAC__stream_decoder_new()) == NULL) {
    fprintf(stderr, "デコーダ割当てエラー\n");
    exit_code = EXIT_FAILURE;
    goto cleaning;
  }
	 
  /* デコーダのインスタンスを初期化する */
  init_status = FLAC__stream_decoder_init_file(dflac.decoder, filePath, write_callback, metadata_callback, 
						error_callback, &dflac);
  if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
    fprintf(stderr, "デコーダ初期化エラー: %s\n", FLAC__StreamDecoderInitStatusString[init_status]);
    exit_code = EXIT_FAILURE;
    goto cleaning;
  }
	
  /* メタデータを読む */
  success = FLAC__stream_decoder_process_until_end_of_metadata (dflac.decoder);
  if(success == false){
    fprintf(stderr, "メタデータのデコード失敗\n");
    fprintf(stderr, "デコーダの状態: %s\n", FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(dflac.decoder)]);
    exit_code = EXIT_FAILURE;
    goto cleaning;
  }	
  playtime = (double)dflac.total_frames / (double)rate ;	/* サウンド再生時間の算出 */ 
 
  switch(dflac.qbits){
  case 16:
    printf("データフォーマット：符号付16bit\n");
    break;
  case 24:
    printf("データフォーマット：符号付24bit\n");	
    break;
  case 32:
    printf("データフォーマット：符号付32bit\n");
    break;
  default:
    fprintf(stderr, "サポート外のデータフォーマット：%d\n", dflac.qbits);
    exit_code = EXIT_FAILURE;
    goto cleaning;
  }
  printf("再生時間：%.0lf秒\n", playtime);
  printf("\n");

  /* ALSAの出力オブジェクト、転送関数、アクセス方法の設定 */
  err = snd_output_stdio_attach(&output, stdout, 0);
  if (err < 0) {
    fprintf(stderr, "ALSAログ出力設定失敗: %s\n", snd_strerror(err));
    exit_code = err;
    goto cleaning;
  }
	
  if (mmap) {
    writei_func = snd_pcm_mmap_writei;
    transfer_method = "mmap_write";
  } else {
    writei_func = snd_pcm_writei;
    transfer_method = "write";
  }
	
  /* PCMをBlockモードでオープンする */
  if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
    fprintf(stderr, "PCMオープンエラー: %s\n", snd_strerror(err));
    exit_code = err;
    goto cleaning;
  }
	
  /* ユーティリティ関数によりPCMにHWパラメータを設定する */
  if ((err = set_hwparams(handle, hwparams)) < 0) {
    fprintf(stderr, "hwparamsの設定失敗: %s\n", snd_strerror(err));
    printf("*** PCMハードウェア構成空間一覧 ***\n");
    snd_pcm_hw_params_dump(hwparams, output);
    printf("\n");
    exit_code = err;
    goto cleaning;
  }
	
  /* ユーティリティ関数によりPCMにSWパラメータを設定する  */
  if ((err = set_swparams(handle, swparams)) < 0) {
    fprintf(stderr, "swparamsの設定失敗: %s\n", snd_strerror(err));
    printf("*** ソフトウェア構成一覧 ***\n");
    snd_pcm_sw_params_dump(swparams, output);
    printf("\n");
    exit_code = err;
    goto cleaning;
  }

  if (verbose > 0){
    printf("*** PCM情報一覧 ***\n");
    snd_pcm_dump(handle, output);
    printf("\n");	
  }
  
  /* ALSAパラメータ情報を表示する */
  printf("*** ALSAパラメータ ***\n");
  printf("内部フォーマット：%s\n", snd_pcm_format_name(format));
  printf("PCMデバイス：%s\n", device);
  printf("転送方法: %s\n", transfer_method);
  printf("\n");

  /* ユーティリティ関数によりファイルからデータを読み、ALSA転送関数に渡してサウンドを再生する */
  err = flac_write_int(handle);
  if (err != 0){
    fprintf(stderr, "再生転送失敗\n");
    exit_code = err;
  }

  /* 後始末 */        	
 cleaning:
  if(init_status = FLAC__STREAM_DECODER_INIT_STATUS_OK)
    FLAC__stream_decoder_finish (dflac.decoder) ;
  if(dflac.decoder != NULL)
    FLAC__stream_decoder_delete(dflac.decoder);
  if(output != NULL)
    snd_output_close(output);
  if(handle != NULL)
    snd_pcm_close(handle);
  snd_config_update_free_global();	
  return exit_code;
}
