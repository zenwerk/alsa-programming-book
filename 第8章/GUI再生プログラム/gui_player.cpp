/********************************************************
 実例プログラム：GUIサウンド・ファイル再生プログラム
 ソースコード：gui_player.c
********************************************************/
#include <getopt.h>
#include <stdbool.h>
#include <pthread.h>
#include "alsa/asoundlib.h"
#include "sndfile.h" 
#include "FL/Fl.H"
#include "FL/Fl_Window.H"
#include "FL/Fl_File_Chooser.H"
#include "FL/Fl_Button.H"
#include "FL/Fl_Box.H"
#include "FL/fl_ask.H"
#include "FL/Fl_Menu_Bar.H"
#include "FL/Fl_Menu_Item.H"
#include "FL/Fl_Choice.H"
#include "FL/Fl_Hor_Value_Slider.H"

/*** ユーティリティ関数プロトタイプ宣言 ***/
static int set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *hwparams);
static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams);
static int gui_write_int(snd_pcm_t *handle);
static void *player(void *arg);
static snd_pcm_sframes_t (*writei_func)(snd_pcm_t *handle, const void *buffer, snd_pcm_uframes_t size);

/*** ALSAライブラリのパラメータ初期化 ***/
static char *device = (char *)"plughw:0,0";		/* 再生デバイス名へのポインタ の初期化*/
static snd_pcm_format_t format = SND_PCM_FORMAT_S32;  	/* サンプル・フォーマット(S32LE) */
static unsigned int rate = 44100;			/* 標本化速度(Hz) */
static unsigned int numChannels = 1;			/* チャンネル数 */
static unsigned int buffer_time = 0;			/* オーディオバッファ長(μsec) */
static unsigned int period_time = 0;			/* データブロック周期(μsec) */
static snd_pcm_uframes_t buffer_size = 0;		/* バッファサイズ(符号無しフレーム数) */
static snd_pcm_uframes_t period_size = 0;		/* データブロック・サイズ(符号無しフレーム数) */

/*** アプリケーション制御パラメータ宣言 ***/
static int mmap = 0;					/* 転送方法制御フラグ: write=0, mmap write=1  */
static int resample = 1;				/* 標本化速度変換設定フラグ: set=1 clear=0 */
static pthread_t play_thread;				/* 再生処理スレッドID */
static char filePath[256] = {0};			/* サウンドファイルパス名 */
static bool isPlay = false;				/* 再生状態識別フラグ */
static bool isStop = false;				/* 停止状態識別フラグ */

/*** libsndfileパラメータの宣言 ***/
static SNDFILE *infile;
static SF_INFO infileInfo;

/*** GUIオブジェクト宣言 ***/
static Fl_Window *MainWindow;	
static Fl_File_Chooser *FileDlg;			/* ファイル選択ダイアログボックス */
static Fl_Choice *PcmDevice;				/* pcmデバイス選択ポップアップ */
static Fl_Box *PlayFile;				/* 再生ファイル名表示ボックス */
static Fl_Box *PlayState;				/* 再生状態表示ボックス */
static Fl_Hor_Value_Slider *TimeBar;			/* 再生時間表示スライダ */

/*** GUIコールバック関数プロトタイプ宣言 ***/
static void cb_loadFile(Fl_Menu_Item *w, void *d);		
static void cb_exit(Fl_Menu_Item *w, void *d);
static void cb_butPlay(Fl_Button *w, void *d);
static void cb_butStop(Fl_Button *w, void *d);
static void cb_pcmDevice(Fl_Choice *w, void *d);

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
    fprintf(stderr, "現static int resample = 1;在のソフトウェアパラメータ確定不可: %s\n", snd_strerror(err));
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
 
/* サウンドデータの再生を行うユーティリティ関数の定義 */
int gui_write_int(snd_pcm_t *handle)
{
  int *bufPtr;						/* 再生フレームバッファ */
  const long numSoundFrames = (long)infileInfo.frames;	/* 再生サウンド総フレーム数 */
  long nFrames, frameCount, numPlayFrames = 0;		/* 再生済フレーム数の初期化 */
  long readFrames, resFrames = numSoundFrames;		/* 未再生フレーム数の初期化 */
  int err = 0; 

  Fl::lock();
  TimeBar->range(0,(double)numSoundFrames/(double)rate);
  Fl::unlock();
	
  /*  オーディオサンプルの転送に適用するデータブロックにメモリを割り当てる  */	
  int *frameBlock = (int *)malloc(period_size * sizeof(int) * numChannels);
  if (frameBlock == NULL) {
    fprintf(stderr, "メモリ不足でデータブロックを割当てられない\n");
    err = EXIT_FAILURE;
    goto cleaning;
  }
  nFrames = (long)period_size; /* サウンドファイルから読み込むフレーム数の初期化 */
  while(resFrames>0 && !isStop){
    readFrames = (long)sf_readf_int(infile, frameBlock, (sf_count_t )nFrames);
    frameCount = readFrames;	/* 書き込むサンプルフレーム数の初期値をサウンドファイルから読み込むフレーム数に設定 */
    bufPtr = frameBlock;	/* 書き込むサンプルのポインタの初期値をフレームブロックの先頭に設定 */
    while (frameCount > 0) {
      err = (int)writei_func(handle, bufPtr, (snd_pcm_uframes_t)frameCount);	/* PCMデバイスにサウンドフレームを転送 */
      if (err == -EAGAIN)
	continue;
      if (err < 0) {
	if (snd_pcm_recover(handle, err, 0) < 0) {
	  fprintf(stderr, "Write転送エラー: %s\n", snd_strerror(err));
	  goto cleaning;
	}
	break;	/* １データブロック周期をスキップ */
      } 
      bufPtr += err *numChannels;	/* フレームバッファのポインタを実際に書いたフレーム数にチャンネル数を乗じた分だけ進める */
      frameCount -= err;		/* フレームバッファ中に残存する書き込み可能なフレーム数を算定 */
    }
    numPlayFrames += readFrames;
    Fl::lock();
    TimeBar->value((double)numPlayFrames/(double)rate);
    Fl::awake();
    Fl::unlock();
		
    /* データ・ブロック長以下の残データフレーム数の計算 */
    if ((resFrames = numSoundFrames - numPlayFrames) <= (long)period_size) 
      nFrames = resFrames;
  }
  snd_pcm_drop(handle);
  if(!isStop){
    Fl::lock();
    PlayState->label("再生終了");
    Fl::awake();
    Fl::unlock();
  }
  printf(" 合計　%lu フレームを再生して終了\n", numPlayFrames);
  err = 0;
 cleaning:
  if(frameBlock != NULL)
    free(frameBlock);
  return err;
}

/* マルチフォーマット再生制御ユーティリティ関数の定義 */
void *player(void *arg)
{
  snd_pcm_t *handle = NULL;		/* PCMハンドル */
  snd_pcm_hw_params_t *hwparams;	/* PCMハードウェア構成空間コンテナ */
  snd_pcm_sw_params_t *swparams; 	/* PCMソフトウェア構成コンテナ */
  unsigned char *transfer_method;	/* 転送方法名 */
  double playtime = 0;
  int err, c;
  int informat, dformat;		/* ファイルフォーマット、データフォーマット */

  /* ALSA HW, SWパラメータ・コンテナの初期化 */
  snd_pcm_hw_params_alloca(&hwparams); 
  snd_pcm_sw_params_alloca(&swparams);
    	
  /* 再生ファイルをオープンする */  	
  if(!(infile = sf_open(filePath, SFM_READ, &infileInfo))){
    fprintf(stderr, "再生ファイル・オープン・エラー: %s\n", sf_strerror(infile));
    goto cleaning;
  }
    	
  /* 再生ファイルのWAVフォーマット情報を取得する */
  informat = infileInfo.format & SF_FORMAT_TYPEMASK;		/* ファイルフォーマットの取得 */
  dformat = infileInfo.format & SF_FORMAT_SUBMASK;		/* データフォーマットの取得 */
  numChannels = infileInfo.channels;				/* チャンネル数の取得 */
  rate = infileInfo.samplerate; 				/* 標本化速度の取得 */ 
  playtime = (double)infileInfo.frames /(double)rate;		/* サウンド再生時間の算出 */ 	
	
  /* 再生ファイルの情報を表示する */
  printf("*** サウンドファイル情報 ***\n");
  printf("ファイル名：%s\n", filePath);
 
  switch(informat) {
  case SF_FORMAT_WAV: 
    printf("ファイルフォーマット：WAVE\n");
    break;
  case SF_FORMAT_WAVEX: 
    printf("ファイルフォーマット：拡張WAVE\n");
    break;
  case SF_FORMAT_AIFF: 
    printf("ファイルフォーマット：AIFF\n");
    break;
  case SF_FORMAT_FLAC: 
    printf("ファイルフォーマット：FLAC\n");
    break; 
  default:
    fprintf(stderr, "サポート外のファイルフォーマット\n");
    goto cleaning;
  }
    	
  switch(dformat){
  case SF_FORMAT_PCM_16: 
    printf("データフォーマット：符号付16bit\n");
    break;
  case SF_FORMAT_PCM_24: 
    printf("データフォーマット：符号付24bit\n");
    break;
  case SF_FORMAT_PCM_32: 
    printf("データフォーマット：符号付32bit\n");
    break;	
  default:
    fprintf(stderr, "サポート外のデータフォーマット\n");
    goto cleaning;
  }
  printf("標本化速度：%dHz\n", rate);
  printf("チャンネル数：%dチャンネル\n", numChannels);
  printf("再生時間：%.0lf秒\n", playtime);
  printf("\n");

  if (mmap) {
    writei_func = snd_pcm_mmap_writei;
    transfer_method = (unsigned char *)"mmap_write";
  } else {
    writei_func = snd_pcm_writei;
    transfer_method = (unsigned char *)"write";
  }
	
  /* PCMをBlockモードでオープンする */
  if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
    fprintf(stderr, "PCMオープンエラー: %s\n", snd_strerror(err));
    goto cleaning;
  }
	
  /* ユーティリティ関数によりPCMにHWパラメータを設定する */
  if ((err = set_hwparams(handle, hwparams)) < 0) {
    fprintf(stderr, "hwparamsの設定失敗: %s\n", snd_strerror(err));
    goto cleaning;
  }
	
  /* ユーティリティ関数によりPCMにSWパラメータを設定する  */
  if ((err = set_swparams(handle, swparams)) < 0) {
    fprintf(stderr, "swparamsの設定失敗: %s\n", snd_strerror(err));
    goto cleaning;
  }
  
  /* ALSAパラメータ情報を表示する */
  printf("*** ALSAパラメータ ***\n");
  printf("内部フォーマット：%s\n", snd_pcm_format_name(format));
  printf("PCMデバイス：%s\n", device);
  printf("転送方法: %s\n", transfer_method);
  printf("\n");

  /* ユーティリティ関数によりファイルからデータを読み、ALSA転送関数に渡してサウンドを再生する */
  isPlay = true;
  err = gui_write_int(handle);
  if (err != 0){
    fprintf(stderr, "再生転送失敗\n");
  }
  
  /* 後始末 */        	
 cleaning:
  isPlay = false;
  isStop = false;
  if(handle != NULL)
    snd_pcm_close(handle);
  snd_config_update_free_global();	
  if(infile != NULL)
    sf_close(infile);
  return((void *)0);
}

/* ファイルロード操作コールバック関数 */
void cb_loadFile(Fl_Menu_Item *w, void *d)
{
  static char currDir[256] = "/home";
  FileDlg->directory(currDir);				/* 現在のディレクトリパスを設定 */
  FileDlg->preview(0);					/* ファイルプレビューを不可 */
  FileDlg->show();					
	
  while (FileDlg->visible())				
    Fl::wait();
  strcpy(currDir,FileDlg->directory());			/* 選択されたディレクトリパスを保存 */
  if (FileDlg->count() > 0 && currDir[0] != '\0') {	
    strcpy (filePath, FileDlg->value(1));		
    PlayFile->label(filePath);				/* サウンドファイル名文字列を更新表示 */
  }
  return;
}

/* プログラム終了操作コールバック関数 */
void cb_exit(Fl_Menu_Item *w, void *d)
{
  MainWindow->hide();
  return;
}

/* 再生ボタン操作コールバック関数 */
void cb_butPlay(Fl_Button *w, void *d)
{
  if(filePath[0] == '\0'){
    fl_message("再生ファイルが未選択！");
    return;
  }
  if(!isPlay){
    int device_index = PcmDevice->value();
    switch(device_index){
    case 0:
      device = (char *)"plughw:0,0";
      break;
    case 1:
      device = (char *)"hw:0,0";
      break;
    case 2:
      device = (char *)"plughw:1,0";
      break;
    case 3:
      device = (char *)"hw:1,0";
      break;
    default:
      break;
    }
    PlayState->label("再生中");
    pthread_create(&play_thread, NULL, player, NULL);
  }
  else 
    fl_message("再生中止するには停止ボタン");
  return;
}

/* 停止ボタン操作コールバック関数 */
void cb_butStop(Fl_Button *w, void *d) 
{
  if (isPlay){
    isStop = true;
    PlayState->label("再生停止");
  }
  return;
}

/* PCMデバイス選択操作コールバック関数 */
void cb_pcmDevice(Fl_Choice *w, void *d)
{
  if(isPlay)
    fl_message("次の再生から設定");
  return;
}

int main(void)
{
  /* ---------- GUI 定義開始 ---------- */
  MainWindow = new Fl_Window(0, 0, 400, 350, "gui_player");			
  Fl_Menu_Item FileItem[] = {
    {"ファイル", 0, 0, 0, FL_SUBMENU, FL_NORMAL_LABEL, 0, 14, 0},
    {"オープン", FL_ALT+'o',  (Fl_Callback *)cb_loadFile, 0, 0, FL_NORMAL_LABEL, 0, 14, 0},
    {"終了", FL_ALT+'x',  (Fl_Callback *)cb_exit, 0, 0, FL_NORMAL_LABEL, 0, 14, 0},
    {0},
    {0},
  };	
  Fl_Menu_Bar *Menu = new Fl_Menu_Bar(0, 0, 80, 35);
  Menu->menu(FileItem);
  PlayFile = new Fl_Box(25, 40, 350, 40, "---再生ファイル---");
  Fl_Menu_Item DeviceItem[] = {
    {"plughw:0,0", 0,  0, 0, 0, FL_NORMAL_LABEL, 0, 14, 0},
    {"hw:0,0", 0,  0, 0, 0, FL_NORMAL_LABEL, 0, 14, 0},
    {"plughw:1,0", 0,  0, 0, 0, FL_NORMAL_LABEL, 0, 14, 0},
    {"hw:1,0", 0, 0, 0, 0, FL_NORMAL_LABEL, 0, 14, 0},
    {0}
  };
  PcmDevice = new Fl_Choice(175, 85, 115, 30, "PCMデバイス");
  PcmDevice->down_box(FL_BORDER_BOX);
  PcmDevice->callback((Fl_Callback *)cb_pcmDevice);
  PcmDevice->menu(DeviceItem);
  PlayState = new Fl_Box(150, 130, 100, 35,"---再生状態---");
  Fl_Button *butPlay = new Fl_Button(90, 180, 90, 45, "@>");
  butPlay->callback((Fl_Callback *)cb_butPlay);
  Fl_Button *butStop = new Fl_Button(220, 180, 90, 45, "@square");
  butStop->callback((Fl_Callback *)cb_butStop);
  new Fl_Box(115, 225, 50, 15, "再生");
  new Fl_Box(245, 225, 50, 15, "停止");	
  TimeBar = new Fl_Hor_Value_Slider(50, 270, 300, 30);
  TimeBar->type(FL_HOR_FILL_SLIDER);
  TimeBar->selection_color(FL_BLUE);
 
  MainWindow->end();
  FileDlg = new Fl_File_Chooser(".", "オーディオファイル (*.{wav,aif,aiff,flac})", Fl_File_Chooser::SINGLE, 
				"オーディオファイルを開く");
  /* ----- GUI 定義終了------ */

  MainWindow->show();	/* ウィンドウを可視化 */	
  Fl::lock();
  return Fl::run();	/* GUIイベントループ実行 */
}
