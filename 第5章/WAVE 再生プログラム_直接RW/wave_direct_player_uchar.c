 /*****************************************************************************
 実例プログラム：WAVEサウンド・ファイル再生プログラム
 		     - mmap_direct転送 -
 ソースコード：wave_direct_player_uchar.c
 ****************************************************************************/
#define _FILE_OFFSET_BITS 64

#include <getopt.h>
#include "alsa/asoundlib.h"
#include "WaveFormat.h"

/*** ユーティリティ関数プロトタイプ宣言 ***/
static int wave_read_header(void);
static int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *hwparams);
static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams);
static int direct_uchar(snd_pcm_t *handle);
static void usage(void);

/*** ALSAライブラリのパラメータ初期化 ***/
static char *device = "plughw:0,0";			/* 再生PCMデバイス名*/
static snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;	/* サンプル・フォーマット */
static unsigned int rate = 44100;			/* 標本化速度(Hz) */
static unsigned int numChannels = 1;			/* チャンネル数 */
static unsigned int buffer_time = 0;			/* バッファ時間長(μsec) */
static unsigned int period_time = 0;			/* 転送周期時間長(μsec) */
static snd_pcm_uframes_t buffer_size = 0;		/* バッファサイズ(符号無しフレーム数) */
static snd_pcm_uframes_t period_size = 0;		/* データブロック・サイズ(符号無しフレーム数) */
static snd_output_t *output = NULL;			/* 出力オブジェクトに 対するALSA内部構造体へのハンドル */

/*** アプリケーション制御フラグの初期化 ***/
static int mmap = 1;					/* 転送方法制御フラグ */
static int verbose = 0;					/* 詳細情報表示フラグ: set=1 clear=0 */
static int resample = 1;				/* 標本化速度変換設定フラグ: set=1 clear=0 */

/*** ユーザデータの宣言 ***/
static WAVEFORMATDESC fmtdesc;
static WAVEFILEDESC filedesc;

/* 再生ファイルからWAVEヘッダのデータを読み込むユーティリティ関数の定義 */
int wave_read_header(void)
{
  FOURCC chunkID;
  DWORD chunkSize;
  GUID SubFormat;
    

  lseek(filedesc.fd, 0, SEEK_SET); 	/* ファイル先頭にrewind */
  /* RIFFチャンクを読む */ 
  read(filedesc.fd, &chunkID, sizeof(FOURCC));
  read(filedesc.fd, &chunkSize, sizeof(DWORD));
  if(chunkID != *(FOURCC *)RIFF_ID) {
    fprintf(stderr, "ファイルエラー：RIFF形式でない\n");
    return EXIT_FAILURE;
  }
 
  /* WAVE IDを読む */  
  read(filedesc.fd, &chunkID, sizeof(FOURCC));
  if(chunkID != *(FOURCC *)WAVE_ID) {
    fprintf(stderr, "ファイルエラー：WAVEフォーマットでない\n");
    return EXIT_FAILURE; 
  }
    
  /* 'fmt 'サブチャンク、'data'サブチャンクの情報を読む */
  while(1){
    read(filedesc.fd, &chunkID, sizeof(FOURCC));
    read(filedesc.fd, &chunkSize, sizeof(DWORD));
    if (chunkID == *(FOURCC *)FMT_ID) {
      if((chunkSize != FORMAT_CHUNK_PCM_SIZE)  && (chunkSize != FORMAT_CHUNK_EX_SIZE)
	 && (chunkSize != FORMAT_CHUNK_EXTENSIBLE_SIZE)) {
	fprintf(stderr, "チャンクサイズ ＝ %d でWAVE規定サイズではない\n", chunkSize);
	return EXIT_FAILURE; 
      } 
    	    			
      /* サウンド・フォーマット情報を読み込む */ 
      read(filedesc.fd, &fmtdesc, FORMAT_CHUNK_PCM_SIZE);
                
      /* formatTagの値を検査する */
      if ((fmtdesc.formatTag != WAVE_FORMAT_PCM) && (fmtdesc.formatTag != WAVE_FORMAT_EXTENSIBLE)) {
	fprintf(stderr, "フォマットコード ＝ %x でPCMフォーマットではない\n", fmtdesc.formatTag);
	return EXIT_FAILURE; 
      }
	    
      /* WAVEフォーマット区分単位で処理 */
      switch(chunkSize){
      case FORMAT_CHUNK_EXTENSIBLE_SIZE:
        lseek(filedesc.fd, 8, SEEK_CUR);
        read(filedesc.fd, &SubFormat, sizeof(GUID));
	if ( SubFormat.subFormatCode != WAVE_FORMAT_PCM){
	  fprintf(stderr, "拡張サブフォマットコード ＝ %x でLPCMフォーマットではない\n",  SubFormat.subFormatCode);
	  return EXIT_FAILURE;
	}
	else if (memcmp(SubFormat.wave_guid_tag, WAVE_GUID_TAG, 14) != 0){
	  fprintf(stderr, "GUIDタグ ＝ %x でWAVE_GUID_TAGではない\n",  (unsigned int)SubFormat.wave_guid_tag);	
	  return EXIT_FAILURE; 
	}
	else{
	  printf("チャンクサイズ　＝　%d で WAVEFORMATEXTENSIBLE 形式のLPCM\n", chunkSize);
	}
	break;
      case FORMAT_CHUNK_EX_SIZE:
        lseek(filedesc.fd, 2, SEEK_CUR);
	printf("チャンクサイズ　＝　%d で WAVEFORMATEX 形式のLPCM\n", chunkSize);
	break;
      default:
	printf("チャンクサイズ　＝　%d で 標準WAVE形式のLPCM\n", chunkSize);
	break;
      }
    }
    else if (chunkID == *(FOURCC *)DATA_ID){
      /* サウンドデータの全フレーム数を設定する */          
      filedesc.frameSize = (long)chunkSize / (long)fmtdesc.dataFrameSize;
      break;
    }
    else{ /* その他のサブチャンクを読み飛ばす */        
      lseek(filedesc.fd, (off_t)chunkSize, SEEK_CUR);
    }                                                
  }
  return 0;
}

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
    buffer_time = 500000;		/* buffer timeの上限を500 msecに設定 */
  if (buffer_time > 0)
    period_time = buffer_time / 4;	/* bufferを4つのperiods(チャンク)に分割 */
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
 
/* サウンドデータの再生を行うユーティリティ関数の定義(SND_PCM_ACCESS_MMAP_INTERLEAVED) */
int direct_uchar(snd_pcm_t *handle)
{
  const snd_pcm_channel_area_t *areas;			/* mmap領域構造体 */
  snd_pcm_uframes_t offset, frames;			/* offset:mmap領域オフセット   */
  snd_pcm_sframes_t avail, transferFrames;
  unsigned short frameBytes =  fmtdesc.dataFrameSize;	/* １フレームのバイト数 */
  const long numSoundFrames = filedesc.frameSize;	/* 再生サウンド総フレーム数 */
  long nFrames, numPlayFrames = 0;	                /* 再生済フレーム数の初期化 */
  long readFrames, resFrames = numSoundFrames;		/* 未再生フレーム数の初期化 */
  unsigned int steps;					/* 隣接サンプル間距離（バイト単位） */
  int err = 0, toStart = 1, i = 0;
  unsigned char *frameBlock ;
  unsigned char *mmap[4];
	
  nFrames = (long)period_size;				/* 一回の転送フレーム数の要求値の初期設定 */
  while(resFrames > 0){
    /* 再生用に書き込み可能なフレーム数を取得する */
    avail = snd_pcm_avail_update(handle);
    if (avail < 0) {
      if ((err = snd_pcm_recover(handle, (int)avail, 0)) < 0) {
	fprintf(stderr, "書き込み可能フレーム取得失敗: %s\n", snd_strerror(err));
	return err;
      }
      toStart = 1;
      continue;
    }
		
    if (avail < nFrames) { 
      if (toStart) {
	toStart = 0;
	err = snd_pcm_start(handle); /* PCMを明示的に開始 */
	if (err < 0) {
	  fprintf(stderr, "PCM開始エラー: %s\n", snd_strerror(err));
	  return err;
	}
      } else {
	err = snd_pcm_wait(handle, -1); /* PCMがready状態になるまで待機 */
	if (err < 0) {
	  if ((err = snd_pcm_recover(handle, err, 0)) < 0) {
	    fprintf(stderr, "PCM待機エラー: %s\n", snd_strerror(err));
	    return err;
	  }
	  toStart = 1;
	}
      }
      continue;
    }
		
    frames = (snd_pcm_uframes_t)nFrames;         
    /* mmap領域へのアクセスを要求する */
    err = snd_pcm_mmap_begin(handle, &areas, &offset, &frames);
    if (err < 0) {
      if ((err = snd_pcm_recover(handle, err, 0)) < 0) {
	fprintf(stderr, "mmap領域アクセス失敗: %s\n", snd_strerror(err));
	return err;
      }
      toStart = 1;
    }
			
    /* 転送データブロックををmmap領域に設定する */   		
     if(i < 4){   		
      mmap[i] = (unsigned char *)areas->addr + (areas->first / 8);	
      steps = areas->step / 8 ;					
      mmap[i] += (unsigned int)offset * steps;
    }
    frameBlock = mmap[i % 4];
    			
    /* 転送データブロック配列にサウンドフレームを読み込む */
    readFrames = (long)(read(filedesc.fd, frameBlock, (size_t)(frames * frameBytes))/frameBytes);
      	  
    /* mmap領域のデータを転送する */
    transferFrames = snd_pcm_mmap_commit(handle, offset, (snd_pcm_uframes_t)readFrames);
    if (transferFrames < 0 || (snd_pcm_uframes_t)transferFrames != frames) {
      if ((err = snd_pcm_recover(handle, transferFrames >= 0 ? -EPIPE : (int)transferFrames, 0)) < 0) {
	fprintf(stderr, "mmap領域コミットエラー: %s\n", snd_strerror(err));
	return err;
      }
      toStart = 1;
    }
   
    numPlayFrames += (long)transferFrames;
		
    /* データ・ブロック長以下の残データフレーム数の計算 */
    if ((resFrames = numSoundFrames - numPlayFrames) <= (long)period_size){ 
      nFrames = resFrames;
    }
    i++;
  }
	
  snd_pcm_drop(handle);
  printf(" 合計 %lu フレームを再生して終了\n", numPlayFrames);
  return 0;
}
 
/* 使用法を表示するユーティリティ関数の定義 */
void usage(void)
{
  int k;
  printf(
	 "使用法: wave_direct_player_uchar [オプション]... [サウンドファイル]...\n"
	 "-h,--help	  使用法\n"
	 "-D,--device	  再生デバイス\n"
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
      {"verbose", 0, NULL, 'v'},
      {"noresample", 0, NULL, 'n'},
      {NULL, 0, NULL, 0},
    };
	
  snd_pcm_t *handle = NULL;				/* PCMハンドル */
  snd_pcm_hw_params_t *hwparams;			/* PCMハードウェア構成空間コンテナ */
  snd_pcm_sw_params_t *swparams;			/* PCMソフトウェア構成コンテナ */
  unsigned char *transfer_method = "mmap_direct";	/* 転送方法名 */                                             
  unsigned short qbits;					/* 量子化ビット数 */
  double playtime = 0;					/* 再生時間 */
  int err, c, exit_code = 0;
		
  while ((c = getopt_long(argc, argv, "hD:vn", long_option, NULL)) != -1) {
    switch (c) {
    case 'h':
      usage();
      return 0;
    case 'D':
      device = strdup(optarg); /* 再生デバイス名の指定 */
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
    	
  /* 再生ファイルパス名の初期化 */
  const char *filePath = NULL;

  /* ALSA HW, SWパラメータ・コンテナの初期化 */
  snd_pcm_hw_params_alloca(&hwparams); 
  snd_pcm_sw_params_alloca(&swparams);
    	
  /* 再生ファイルをオープンする */
  filePath = argv[optind];
  int fd = open(filePath, O_RDONLY, 0);
  if(fd == -1){
    fprintf(stderr, "再生ファイル・オープン・エラー\n");
    exit_code = errno;
    goto cleaning;
  }
  filedesc.fd = fd;
    	
  /* ユーティリティ関数により再生ファイルのWAVフォーマット情報を取得する */
  if(wave_read_header() != 0){
    exit_code = EXIT_FAILURE;
    goto cleaning;
  }
     
  if(fmtdesc.bitsPerSample > 32) {
    fprintf(stderr, "サポート外の量子化ビット数：%d\n", fmtdesc.bitsPerSample );
    exit_code = EXIT_FAILURE;
    goto cleaning;
  }
   	 	
  numChannels = (unsigned int)fmtdesc.numChannels;	/* チャンネル数の取得 */
  rate = fmtdesc.samplesPerSec;				/* 標本化速度の取得 */
  qbits = fmtdesc.bitsPerSample;			/* 量子化ビット数の取得 */
  playtime = (double)filedesc.frameSize / (double)rate;	/* サウンド再生時間の算出 */
	
  /* 再生ファイルの情報を表示する */
  printf("*** サウンドファイル情報 ***\n");
  printf("ファイル名：%s\n", filePath);
  printf("標本化速度：%dHz\n", rate);
  printf("チャンネル数：%dチャンネル\n", numChannels);
 
  switch(qbits){
  case 16:
    format = SND_PCM_FORMAT_S16_LE;
    printf("データフォーマット：符号付16bit\n");
    break;
  case 24:	
    format = SND_PCM_FORMAT_S24_3LE;
    printf("データフォーマット：符号付24bit\n");
    break;
  case 32:	
    printf("データフォーマット：符号付32bit\n");
    break;
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
  err = direct_uchar(handle);
  if (err != 0){
    fprintf(stderr, "再生転送失敗\n");
    exit_code = err;
  }

  /* 後始末 */        	
 cleaning:
  if(output != NULL)
    snd_output_close(output);
  if(handle != NULL)
    snd_pcm_close(handle);
  snd_config_update_free_global();	
  if(fd != -1)
    close (fd);
  return exit_code;
}
