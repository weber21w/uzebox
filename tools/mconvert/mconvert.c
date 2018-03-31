/*
 *  Uzebox(tm Alec Bourque) mconvert utility
 *  2017 Lee Weber
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Uzebox is a reserved trade mark
*/

/* This tool converts standard midiconv output into a compressed version
 * as C array or binary(can be placed in existing resource file) for use
 * by the streaming music player. It is intended to improve efficiency
 * over the old MIDI format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FILE *fin, *fout, *fcfg;

int asBin;
int doPad;
int doDebug;
int doLength;
int doCustomName;
char arrayName[256];
char finName[256];
char foutName[256];
int ctrFilter;
char lineBuf[256];
int eat;
int padBytes;

long outOff;
long fileOff;
long dirOff;
long runTime;
long minSize;
long flashCost;
long totalFlashCost;
int inSize,outSize,cfgSize,cfgLine,cfgEntry;
unsigned char inBuf[1024*128];
unsigned char outBuf[1024*128];
unsigned char cfgBuf[1024*4];

long loopStart;
unsigned char loopBuf[256];

#define FILTER_CHANNEL_VOLUME	1
#define FILTER_EXPRESSION	2
#define FILTER_TREMOLO_VOLUME	4
#define FILTER_TREMOLO_RATE	8
#define FILTER_NOTE_OFF		16

int ConvertAndWrite();

int main(int argc, char *argv[]){

	int i,j;

	if(argc < 2){
		printf("mconvert - compressed MIDI convertor for Uzebox\n");
		printf("\tconverts C array files generated by midiconv, and outputs\n");
		printf("\tcompressed versions for the streaming music player\n");
		printf("\tusage:\n\t\t\tmconvert config.cfg\n");
		goto DONE;
	}

	fcfg = fopen(argv[1],"r");
	if(fcfg == NULL){
		printf("Error: Failed to open config file: %s\n",argv[1]);
		goto ERROR;
	}

	while(1){/* eat any new lines the user inserted before the setup line */
		if(fgets(lineBuf,sizeof(lineBuf)-1,fcfg) == NULL){//read the initial line
			printf("Error: failed to read parameter line\n");
			goto ERROR;
		}
		if(lineBuf[0] != '\r' && lineBuf[0] != '\n'){/* found junk before the setup line? */
			if(lineBuf[0] == '#'){//it is a comment line, eat it
				for(j=1;j<sizeof(lineBuf);j++){
					if(lineBuf[j] == '\n')
						break;
					if(j == sizeof(lineBuf)-1){
						printf("Error: failed to find setup line\n");
						goto ERROR;
					}
				}
				continue;
			}else
				break;/* got garbage, let it fail */
		}else if((lineBuf[0] == '\r' && lineBuf[1] == '\n') || lineBuf[0] == '\n')/* found a Windows or Unix style line ending? Eat it */
			continue;
	}

	if(sscanf(lineBuf," %d , %ld , %ld , %d , %255[^ ,\n\t] , %n ",&asBin,&dirOff,&fileOff,&doPad,foutName,&eat) != 5){
		printf("Error: bad format on setup line. Got \"%s\"\n",lineBuf);
		printf("\tFormat is 5 comma separated entries, as:\n\t\t0/1 = C array or binary output,\n\t\tdirectory start offset,\n\t\tsong data start offset,\n\t\t0/1 = pad songs to 512 byte sector size,\n\t\toutput file name,\n");
		printf("\n\tEx: 1,0,512,1,OUTPUT.DAT\n\tbinary, dir at 0, starting at 512, padded, to OUTPUT.DAT\n");
		goto ERROR;
	}

	printf(asBin ? "\n\tBinary output, ":"\n\tC array output");
	if(asBin){
		if(dirOff >= 0)
			printf("directory at %ld, data at  %ld, to %s:\n",dirOff,fileOff,foutName);
		else
			printf("directory output disabled\n");
	}else
		printf(" to %s:\n",foutName);

	fout = fopen(foutName,asBin ? "rb+":"w");/* non-binary destroys any previous C array output */
	if(fout == NULL){/* file does not exist, try to create it. */
		printf("\tBinary output file does not exist");
		if(asBin){
			fout = fopen(foutName,"wb");
			if(fout == NULL){
				printf("\nError: Failed to create output file: %s\n",foutName);
				goto ERROR;
			}
			printf(", created %s\n",foutName);
			if(outOff)
				printf("Padding new binary file with %ld '0's\n",outOff);
			for(i=0;i<outOff;i++)/* pad the new file with 0 up to the specified offset */
				fputc(0,fout);
		}else{/* the "w" should have succeeded */
			printf("Error: Failed to create output file: %s\n",foutName);
			goto ERROR;
		}

	}

	cfgLine = 0;
	cfgEntry = 0;
	while(!feof(fcfg)){
		cfgLine++;

		if(fgets(lineBuf,sizeof(lineBuf)-1,fcfg) == NULL){//read the next line
			if(cfgEntry == 1)
				printf("\t== 1 entry total");
			else
				printf("\t== %d entries total",cfgEntry);
			if(asBin)
				printf(", %ld bytes file size\n",fileOff);
			else
				printf(", %ld bytes(total flash cost)\n",totalFlashCost);
			goto DONE;
		}

		sprintf(arrayName,"song%d",cfgEntry);
		if(sscanf(lineBuf," %255[^ ,\t] , %d ,  %32[^ ,\t] %n",finName,&ctrFilter,arrayName,&eat) != 3){
			if(lineBuf[0] == '\r' || lineBuf[0] == '\n'){/* user entered an extra line end after the entries, eat it */
				continue;
			}else if(lineBuf[0] == '#'){//eat the comment line
				for(j=1;j<sizeof(lineBuf);j++){
					if(lineBuf[j] == '\n')
						break;
					if(j == sizeof(lineBuf)-1){
						printf("Error: did not find end of comment for line %d\n",cfgLine);
						goto ERROR;
					}
				}
				continue;
			}else{
				printf("Error: bad format on entry %d line %d. Got \"%s\"\n",cfgEntry+1,cfgLine+1,lineBuf);
				goto ERROR;
			}
		}

		cfgEntry++;
		printf("\t+= %s,filter:%d,offset:%ld\n",finName,ctrFilter,fileOff);

		fin = fopen(finName,"r");
		if(fin == NULL){
			printf("Error: Failed to open input file: %s\n",finName);
			goto ERROR;
		}

		i = ConvertAndWrite();

		if(i != 1){
			printf("Error: conversion failed for %s on entry %d, line %d, error: %d\n",finName,cfgEntry,cfgLine,i);
			goto ERROR;
		}
	}

	goto DONE;
ERROR:
	exit(1);
DONE:	
	exit(0);
}



int ConvertAndWrite(){
	int foundSongEnd,delta,i,j,w;
	unsigned char c1,c2,c3,t,lastStatus,channel;
	w = 0;
	loopStart = -1;
	inSize = 0;
	outSize = 0;
	lastStatus = 0;
	runTime = 0;
	foundSongEnd = 0;
	channel = 0;
	flashCost = 0;

	while(fgetc(fin) != '{' && !feof(fin));/* eat everything up to the beginning of the array data */

	if(feof(fin))/* got to the end of the file without seeing the opening bracket of the array */
		return -1;

	while(fscanf(fin," 0%*[xX]%x , ",&i) == 1)
		inBuf[inSize++] = (i & 0xFF);

	i = 1; /* skip first delta */

	while(i < inSize){
		if(i > sizeof(outBuf)-10){
			printf("Error: outBuf out of bounds\n");
			return -2;
		}

		c1 = inBuf[i++];

		if(c1 == 0xFF){/* a meta event */
			c1 = inBuf[i++];
			if(c1 == 0x2F){/* end of song */
				outBuf[outSize++] = 		0b11000010;/* Song End */
				foundSongEnd = 1;
				break;

			}else if(c1 == 0x06){ /* Loop markers or unsupported command */
				c1 = inBuf[i++]; /* eat len byte */
				c2 = inBuf[i++]; /* get data */				
				if(c2 == 'S'){
					outBuf[outSize++] = 	0b11000001;/* Loop Start */
					loopStart = outSize;
				}else if(c2 == 'E'){
					outBuf[outSize++] = 	0b11000000;/* Loop End */
					foundSongEnd = 1;
					break;
				}else{/* something that midiconv should not have output */
					printf("Error: Got an unrecognized command\n");		
					return -4;
				}
			}

		}else{
			if(c1 & 0x80)
				lastStatus = c1;
			channel = (lastStatus & 0x0F);
			if(channel > 4){
				printf("\nError: Got bad channel:%d from byte:%d at offset %d\n",channel,c1,outSize);
				return -5;
			}

			if(c1 & 0x80)
				c1 = inBuf[i++];

			switch(lastStatus & 0xF0){

				case 0x90: /* Note On event, c1 = note, 2 bytes */
					/* 0b1CCCVVVV, 0bVNNNNNNN */
					c2 = inBuf[i++];/* c2 = 7 bit volume */
					c2 >>= 1; /* convert to 6 bit volume used in the compressed format */
					if(c2 || !(ctrFilter & FILTER_NOTE_OFF)){/* Note Off is a Note On with volume 0 */
						outBuf[outSize++] = (channel<<5) | (c2 & 0b00011111);/* 0bCCCVVVVV */
						outBuf[outSize++] = ((c2 & 0b00100000)<<2) | (c1 & 0b01111111);/* 0bVNNNNNNN MSBit of volume*/
					}			
					break;

				case 0xB0:/* controller, c1 = type, 2 bytes */
					c2 = inBuf[i++];/* c2 = controller value */
					/* 0b111XXCCC */
					if(c1 == 0x07){/* channel volume */
						if(!(ctrFilter & FILTER_CHANNEL_VOLUME))
							outBuf[outSize++] = 0b11100000 | channel;
						else
							break;/* don't write controller value */
					}else if(c1 == 0x0B){ /* expression */
						if(!(ctrFilter & FILTER_EXPRESSION))
							outBuf[outSize++] = 0b11101000 | channel;
						else
							break;
					}else if(c1 == 0x5C){ /* tremolo volume */
						if(!(ctrFilter & FILTER_TREMOLO_VOLUME))
							outBuf[outSize++] = 0b11110000 | channel;
						else
							break;
					}else if(c1 == 0x64){ /* tremolo rate */
						if(!(ctrFilter & FILTER_TREMOLO_RATE))
							outBuf[outSize++] = 0b11111000 | channel;
						else
							break;
					}else{/* got something that midiconv should not have output */
						printf("Error: Got unknown controller event\n");
						return -6;
					}

					outBuf[outSize++] = c2;/* controller value */
					break;

				case 0xC0:/* program change, c1 = patch */
						outBuf[outSize++] = 0b10100000 | channel;/*0b10100CCC*/
						outBuf[outSize++] = c1; /* patch */
					break;
				default:
					printf("Error: Got unknown controller\n");
					return -1;
			}/* end switch(lastStatus & 0x0F) */
		}/* end else(c1 != 0xFF) */

		delta = inBuf[i++]; /* calculate the next delta time, which is possibly encoded as multiple bytes */

		if(delta & 0x80){
			delta &= 0x7F;
			do{
				c3 = inBuf[i++];
				delta = (delta<<7) + (c3 & 0x7F);
			}while(c3 & 0x80);
		}

		if(delta == 0)/* we do not store deltas of 0(it is implied in the format) */
			continue;

		runTime += delta;/* keep track of how long this song is, in 60hz ticks, for statistic display */

		while(delta){
			/* for a non-zero delta, we create a Tick End event */
			if(delta < 8){/* store 1 to 7 frame delays in the same byte as the command, 0 = 1, 1 = 2, etc. */
				outBuf[outSize++] = 0b11000011 | (((delta-1) & 0b00000111)<<2);/* 0b110DDD11, stored as 1 less than actual delay */
				delta = 0;
			}else{ /* we store longer delays in a 2 byte format */
				outBuf[outSize++] = 0b11011111;/* delta of 7(+1) indicates that the next byte holds the actual delay */
				if(delta > 254){/* we can store any delay as a series of 8 bit values */
					outBuf[outSize++] = 0b11111111;
					delta -= 254;
				}else{
					outBuf[outSize++] = (delta & 0b11111111);
					delta = 0;
				}
			}
		}



	}

	if(i >= inSize)/* the data left some command incomplete, ie. extra or not enough bytes */
		return -3;

	if(!foundSongEnd){
		printf("Error: Did not find song or loop end point\n");
		return -2;
	}

	if(asBin && loopStart >= 0)//do not put the loop buffer in the C array, which is likely intended for the non-buffered player(save space)	
		for(i=0;i<sizeof(loopBuf);i++)
			outBuf[outSize++] = outBuf[i+loopStart];

	padBytes = 0;
	if(doPad)/* pad out the size to fill the full sector */
		while(outSize%512){
			outBuf[outSize++] = 0xFF;/* forces a song end if reached */
			padBytes++;
		}

	if(asBin){
		if(dirOff >= 0){//user can omit directory information by passing a negative offset
			fseek(fout,dirOff,SEEK_SET);/* write the directory entry for this song */
			/* the byte order matches what is expected by SpiRamReadU32() */
			fputc(((unsigned char)(fileOff>>0)&0xFF),fout);
			fputc(((unsigned char)(fileOff>>8)&0xFF),fout);
			fputc(((unsigned char)(fileOff>>16)&0xFF),fout);
			fputc(((unsigned char)(fileOff>>24)&0xFF),fout);

			dirOff += 4;
		}

		fseek(fout,fileOff,SEEK_SET);
		if(doLength){/* prepend the total data size before the song data(useful for loading to SPI ram) */
			fputc((outSize>>8)&0xFF,fout);
			fputc((outSize>>0)&0xFF,fout);
			fileOff += 2;
		}
		for(i=0;i<outSize;i++){/* output the actual data, text or binary, and any offset were already setup prior */
			fputc(outBuf[i],fout);
			fileOff++;
		}

	}else{/* C array */
		fprintf(fout,"const char %s[] PROGMEM = {/* %s */\n",arrayName,finName);
		w = 0;
		for(i=0;i<outSize-padBytes;i++){/* user added padding for C version? Seems no use, assume it is in error and override. */
			if(!doDebug)
				fprintf(fout,"0x%02X,",outBuf[i]);
			else{
				fprintf(fout,"0b");
				for(j=0;j<8;j++)
					fputc(((outBuf[i]<<j)&128) ? '1':'0',fout);
				fputc(',',fout);
				w = 100;
			}
			if(++w > 15){
				w = 0;
				fprintf(fout,"\n");
			}
			flashCost++;
		}
		if(w != 0)/* make formatting nicer */
			fprintf(fout,"\n");
		fprintf(fout,"};/* %ld bytes total */\n\n",flashCost);
		totalFlashCost += flashCost;
	}
	/* display statistics */
	printf("\t\tRun time: %ld seconds\n",(runTime/60));
	printf("\t\tInput data size: %d\n",inSize);
	if(loopStart == -1){
		if(asBin)
			printf("\t\tOutput data size: %ld(+0 loop +%d pad)\n",(long)(outSize-padBytes),padBytes);
		else
			printf("\t\tOutput data size: %ld(no loop)\n",flashCost);
	}else{
		if(asBin)
			printf("\t\tOutput data size: %ld(+%d loop +%d pad)\n",(long)(outSize-sizeof(loopBuf)-padBytes),sizeof(loopBuf),padBytes);
		else
			printf("\t\tOutput data size: %ld\n",flashCost);//C array is meant for non-buffered player(save space)
	}
	printf("\t\tAverage bytes per frame: %f\n",(((double)outSize-(double)padBytes)/(double)runTime));
	printf("\t\tAverage bytes per second: %f\n\n",((((double)outSize-(double)padBytes)/((double)runTime/60))));

	return 1;
}
