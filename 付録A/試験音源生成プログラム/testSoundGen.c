/************************************************
実例プログラム：試験用音源生成プログラム
ソースコード：testSoundGen.c
************************************************/
#include <stdio.h>
#include <getopt.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "sndfile.h" 

static int fs = 44100;             	/* 標本化周波数(Hz) */
static int numBits = 16;		/* 量子化ビット数 */
static int numChannels = 2;             /* チャンネル数（ステレオ固定） */

/* 使用法を表示するユーティリティ関数の定義 */
static void usage(void)
{
  int k;
  printf(
	 "Usage: testSoundGen [オプション]...\n"
	 "-h,--help	  使用法\n"
	 "-f,--format	  ファイル拡張子: (デフォルトwav), オプション flacまたはaiff\n"
	 "-r,--rate	  標本化速度: 最小44100(デフォルト),最大192000\n"
	 "-b,--bits         量子化ビット数: 16(デフォルト), 24, 32\n"
	 "\n");
  printf("適用ファイルフォーマット:WAVE, FLAC, AIFF\n");
  printf("\n");
}

int main(int argc, char *argv[])
{
  static const struct option long_option[] =
    {
      {"help", 0, NULL, 'h'},
      {"format", 1, NULL, 'f'},
      {"rate", 1, NULL, 'r'},
      {"bits", 1, NULL, 'b'},
      {NULL, 0, NULL, 0},
    };

  /* (1) 変数定義 */
  int c;
  char *formatID[3] = {"16", "44100", "wav"};		
  char filename[256] = "tone2_";	/* 音源ファイル名 */                 
  int     sample[2];               	/* サウンド標本(整数型）フレーム */
  int     numSamples;       	 	/* 標本数 */
  float   duration = 5.0;        	/* 再生時間(秒) */
  float   frequency = 1000.0;		/* 純音の周波数(Hz) */
  double  fsample;			/* サウンド標本(浮動少数点型） */
  double  pi = 4 * atan(1.);		/* 円周率 */
  double  start = 1.0;			/* 標本初期値(0dB) */
  double  end = 1.0e-4; 		/* 標本最終値(-80dB)*/                                                                                               
  double  current;			/* 標本現在値 */  
  double  factor;			/* 振幅減衰係数 */  
  double  angleIncrement;		/* 純音の位相角の増分 */
    
  /* libsndfile */
  SNDFILE *outfile;
  SF_INFO outfileInfo = {0};
  int subformat, format = SF_FORMAT_WAV;
             
  /* (2) コマンドライン・オプション処理 */
  while ((c = getopt_long(argc, argv, "hf:r:b:", long_option, NULL)) != -1) {
    switch (c) {
    case 'h':
      usage();
      return 0;
    case 'f':
      formatID[2] = strdup(optarg); 
      if (strcmp(formatID[2], "flac") == 0){
	format = SF_FORMAT_FLAC;
      }else if (strcmp(formatID[2], "aiff") == 0){
	format = SF_FORMAT_AIFF;
      }else{
	usage();
	exit(-1);
      }
      break;
    case 'r':
      fs = atoi(optarg);
      if ((fs > 192000) || (fs < 44100)){
	usage();
	exit(-1);
      }
      formatID[1] = strdup(optarg);		
      break;
    case 'b':
      numBits = atoi(optarg);
      if (!((numBits == 32)||(numBits == 24)||(numBits == 16))){
	usage();
	exit(-1);
      }
      formatID[0] = strdup(optarg);
      break;	
    default:
      fprintf(stderr, "`--help'で使用方法を確認\n");
      exit(-1);
    }
  }
  
  /* (3) 音源ファイル名生成 */
  strcat(filename, formatID[0]);
  strcat(filename, "_");
  strcat(filename, formatID[1]);
  strcat(filename, ".");
  strcat(filename, formatID[2]);
 
  /* (4) libsndfileデータフォーマット設定 */
  switch (numBits) {
  case 16:
    subformat = SF_FORMAT_PCM_16;
    break;
  case 24:
    subformat = SF_FORMAT_PCM_24;
    break;
  case 32:
    if (format == SF_FORMAT_FLAC){
      fprintf(stderr, "FLAC形式では32bitをサポートしていません\n");
      exit(-1);
    }
    subformat = SF_FORMAT_PCM_32;
    break;
  }
    
  /* (5) 再生純音を規定するパラメータ算出 */
  numSamples = (int)(duration * fs);
  angleIncrement = 2. * pi * frequency / fs;
  factor = pow(end/start, 1.0/numSamples); /* pow(x,y):xのy乗 , <math.h> */

  /* (6) 音源ファイルのパラメータ設定 */
  outfileInfo.samplerate = fs;
  outfileInfo.channels = numChannels;
  outfileInfo.format = format | subformat;
  if(!(outfile = sf_open(filename, SFM_WRITE, &outfileInfo))){
    fprintf(stderr, "出力ファイルオープン失敗\n");
    exit(-1);	
  }
    
  /* (7) 単調に減衰する純音データ標本を音源ファイルにwrite */
  current = start;
  for(int i=0; i < numSamples; i++){
    fsample = current * sin(angleIncrement*i);
    current *= factor;
    sample[0] = sample[1] = (int)(INT_MAX * fsample); /* INT_MAX:int型の最大値 */
    sf_write_int(outfile, sample, 2);
  }
  printf("%d サンプルをファイル %s に書く\n", numSamples, filename);
    
  /* (8) 資源を開放して、プログラムを終了 */
  if(outfile != NULL)
    sf_close(outfile);
  return 0;
}

