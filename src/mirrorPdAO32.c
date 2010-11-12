/*
darc, the Durham Adaptive optics Real-time Controller.
Copyright (C) 2010 Alastair Basden.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
   The code here is used to create a shared object library, which can then be swapped around depending on which mirrors/interfaces you have in use, ie you simple rename the mirror file you want to mirror.so (or better, change the soft link), and restart the coremain.

The library is written for a specific mirror configuration - ie in multiple mirror situations, the library is written to handle multiple mirrors, not a single mirror many times.

This library is written for the DAC card.  For use in a figure sensing RTC (or one which has the DAC card locally).

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "rtcmirror.h"
#include <time.h>
#include <pthread.h>
#include "powerdaq.h"
#include "powerdaq32.h"
#include "darc.h"

#define errorChk(functionCall) {int error; if((error=functionCall)<0) { \
	                           fprintf(stderr, "Error %d at line %d in function call %s\n", error, __LINE__, #functionCall); \
	                           exit(EXIT_FAILURE);}}
typedef unsigned int uint32;
typedef enum _state
{
   closed,
   unconfigured,
   configured,
   running
} tState;

typedef enum{
  MIRRORACTINIT,
  MIRRORACTMAPPING,
  MIRRORACTMAX,
  MIRRORACTMIN,
  MIRRORNACTS,
  //Add more before this line.
  MIRRORNBUFFERVARIABLES//equal to number of entries in the enum
}MIRRORBUFFERVARIABLEINDX;
char *MIRRORPARAM[]={"nacts","actMin","actMax","actMapping","actInit"};
#define makeParamNames() bufferMakeNames(MIRRORNBUFFERVARIABLES,\
					 "actInit","actMapping","actMax","actMin","nacts")


typedef struct{
  unsigned short *actMin;
  unsigned short *actMax;
  int actMinSize;
  int actMaxSize;
  int *actMapping;
  int actMappingSize;
  int useActMapping;
}MirrorStructBuffered;



typedef struct{
  int nacts;
  unsigned short *arr;
  uint32 arrsize;
  int open;
  int err;
  pthread_t threadid;
  pthread_cond_t cond;
  pthread_mutex_t m;
  int handle;
  int threadAffinity;
  int threadPriority;
  tState state;//state of the acquisition session.
  int board;
  int adapterType;
  MirrorStructBuffered msb[2];
  int buf;
  int swap;
  //int bufindx[MIRRORNBUFFERVARIABLES];
  circBuf *rtcActuatorBuf;
  circBuf *rtcErrorBuf;
  unsigned short *actInit;
  int initLen;
  char *paramNames;
  int index[MIRRORNBUFFERVARIABLES];
  void *values[MIRRORNBUFFERVARIABLES];
  char dtype[MIRRORNBUFFERVARIABLES];
  int nbytes[MIRRORNBUFFERVARIABLES];

}MirrorStruct;


int InitSingleAO(MirrorStruct *pAoData){
   Adapter_Info adaptInfo;
   // get adapter type
   errorChk(_PdGetAdapterInfo(pAoData->board, &adaptInfo));
   pAoData->adapterType = adaptInfo.atType;
   if(pAoData->adapterType & atMF)
     printf("This is an MFx board\n");
   else
     printf("This is an AO32 board\n");
   pAoData->handle = PdAcquireSubsystem(pAoData->board, AnalogOut, 1);
   if(pAoData->handle < 0){
      printf("SingleAO: PdAcquireSubsystem failed\n");
      pAoData->state=closed;
      return 1;
   }
   pAoData->state = unconfigured;
   errorChk(_PdAOutReset(pAoData->handle));
   // need also to call this function if the board is a PD2-AO-xx
   if(pAoData->adapterType & atPD2AO){
      errorChk(_PdAO32Reset(pAoData->handle));
   }
   return 0;
}




void CleanUpSingleAO(MirrorStruct *pAoData){
   if(pAoData->state == running){
      pAoData->state = configured;
   }
   if(pAoData->state == configured){
     errorChk(_PdAOutReset(pAoData->handle));
     // need also to call this function if the board is a PD2-AO-xx
     if(pAoData->adapterType & atPD2AO){
       errorChk(_PdAO32Reset(pAoData->handle));
     }
     pAoData->state = unconfigured;
   }
   if(pAoData->handle > 0 && pAoData->state == unconfigured){
     errorChk(PdAcquireSubsystem(pAoData->handle, AnalogOut, 0));
   }
   pAoData->state = closed;
}


/**
   Free mirror/memory/sl240
*/
void mirrordofree(MirrorStruct *mirstr){
  int i;
  if(mirstr!=NULL){
    if(mirstr->arr!=NULL)
      free(mirstr->arr);
    for(i=0; i<2; i++){
      if(mirstr->msb[i].actMin!=NULL)free(mirstr->msb[i].actMin);
      if(mirstr->msb[i].actMax!=NULL)free(mirstr->msb[i].actMax);
      if(mirstr->msb[i].actMapping!=NULL)free(mirstr->msb[i].actMapping);
    }

    CleanUpSingleAO(mirstr);
    pthread_cond_destroy(&mirstr->cond);
    pthread_mutex_destroy(&mirstr->m);
    free(mirstr);
  }
}



int mirrorsetThreadAffinity(int threadAffinity,int threadPriority){
  int i;
  cpu_set_t mask;
  int ncpu;
  struct sched_param param;
  ncpu= sysconf(_SC_NPROCESSORS_ONLN);
  CPU_ZERO(&mask);
  for(i=0; i<ncpu; i++){
    if(((threadAffinity)>>i)&1){
      CPU_SET(i,&mask);
    }
  }
  //printf("Thread affinity %d\n",threadAffinity&0xff);
  if(sched_setaffinity(0,sizeof(cpu_set_t),&mask))
    printf("Error in sched_setaffinity: %s\n",strerror(errno));
  param.sched_priority=threadPriority;
  if(sched_setparam(0,&param)){
    printf("Error in sched_setparam: %s - probably need to run as root if this is important\n",strerror(errno));
  }
  if(sched_setscheduler(0,SCHED_RR,&param))
    printf("sched_setscheduler: %s - probably need to run as root if this is important\n",strerror(errno));
  if(pthread_setschedparam(pthread_self(),SCHED_RR,&param))
    printf("error in pthread_setschedparam - maybe run as root?\n");
  return 0;
}

/**
   The thread that does the work - copies actuators, and sends to the DAC
*/
void* worker(void *mirstrv){
  MirrorStruct *mirstr=(MirrorStruct*)mirstrv;
  int i;
  mirrorsetThreadAffinity(mirstr->threadAffinity,mirstr->threadPriority);
  pthread_mutex_lock(&mirstr->m);
  if(mirstr->open && mirstr->actInit!=NULL){
    for(i=0; i<mirstr->initLen; i++){
      _PdAO32Write(mirstr->handle,i,mirstr->actInit[i]);
    }
  }
  
  while(mirstr->open){
    pthread_cond_wait(&mirstr->cond,&mirstr->m);//wait for actuators.
    if(mirstr->open){
      //Now send the header...
      mirstr->err=0;
      if(mirstr->msb[mirstr->buf].useActMapping==1 && mirstr->msb[mirstr->buf].actMapping!=NULL){
	for(i=0; i<mirstr->nacts; i++){
	  mirstr->err|=_PdAO32Write(mirstr->handle,mirstr->msb[mirstr->buf].actMapping[i],mirstr->arr[i]);
	}
      }else{
	for(i=0; i<mirstr->nacts; i++){
	  mirstr->err|=_PdAO32Write(mirstr->handle,i,mirstr->arr[i]);
	}
      }
    }
  }
  pthread_mutex_unlock(&mirstr->m);
  return NULL;
}

/**
   Open a camera of type name.  Args are passed in a float array of size n, which can be cast if necessary.  Any state data is returned in camHandle, which should be NULL if an error arises.
   pxlbuf is the array that should hold the data. The library is free to use the user provided version, or use its own version as necessary (ie a pointer to physical memory or whatever).  It is of size npxls*sizeof(short).
   ncam is number of cameras, which is the length of arrays pxlx and pxly, which contain the dimensions for each camera.
   Name is used if a library can support more than one camera.

*/

int mirrorOpen(char *name,int narg,int *args,paramBuf *pbuf,circBuf *rtcErrorBuf,char *prefix,arrayStruct *arr,void **mirrorHandle,int nacts,circBuf *rtcActuatorBuf,unsigned int frameno,unsigned int **mirrorframeno,int *mirrorframenoSize){
  int err;
  MirrorStruct *mirstr;
  DWORD aoCfg;
  char *pn;
  printf("Initialising mirror %s\n",name);
  if((pn=makeParamNames())==NULL){
    printf("Error making paramList - please recode mirrorPdAO32.c\n");
    *mirrorHandle=NULL;
    return 1;
  }


  if((*mirrorHandle=malloc(sizeof(MirrorStruct)))==NULL){
    printf("couldn't malloc mirrorHandle\n");
    return 1;
  }
  mirstr=(MirrorStruct*)*mirrorHandle;
  memset(mirstr,0,sizeof(MirrorStruct));
  mirstr->paramNames=pn;
  mirstr->nacts=nacts;
  mirstr->rtcErrorBuf=rtcErrorBuf;
  mirstr->rtcActuatorBuf=rtcActuatorBuf;
  mirstr->arrsize=nacts*sizeof(unsigned short);
  if((mirstr->arr=malloc(mirstr->arrsize))==NULL){
    printf("couldn't malloc arr\n");
    mirrordofree(mirstr);
    *mirrorHandle=NULL;
    return 1;
  }
  memset(mirstr->arr,0,mirstr->arrsize);
  if(narg==2){
    mirstr->threadAffinity=args[0];
    mirstr->threadPriority=args[1];
  }else{
    printf("wrong number of args - should be thread affinity, thread priority\n");
    mirrordofree(mirstr);
    *mirrorHandle=NULL;
    return 1;
  }

  if(pthread_cond_init(&mirstr->cond,NULL)!=0){
    printf("Error initialising thread condition variable\n");
    mirrordofree(mirstr);
    *mirrorHandle=NULL;
    return 1;
  }
  //maybe think about having one per camera???
  if(pthread_mutex_init(&mirstr->m,NULL)!=0){
    printf("Error initialising mutex variable\n");
    mirrordofree(mirstr);
    *mirrorHandle=NULL;
    return 1;
  }
  //initialise acquisition session
  if(InitSingleAO(mirstr)){//failed...
    printf("Failed to initSingleAO\n");
    mirrordofree(mirstr);
    *mirrorHandle=NULL;
    return 1;
  }
  // set configuration - _PdAOutReset is called inside _PdAOutSetCfg
  aoCfg = 0;
  errorChk(_PdAOutSetCfg(mirstr->handle, aoCfg, 0));
  mirstr->state = configured;
  //Start SW trigger
  errorChk(_PdAOutSwStartTrig(mirstr->handle));
  mirstr->state = running;

  mirstr->open=1;
  if((err=mirrorNewParam(*mirrorHandle,pbuf,frameno,arr))){
    mirrordofree(mirstr);
    *mirrorHandle=NULL;
    return 1;
  }
  pthread_create(&mirstr->threadid,NULL,worker,mirstr);
  return 0;
}

/**
   Close a camera of type name.  Args are passed in the float array of size n, and state data is in camHandle, which should be freed and set to NULL before returning.
*/
int mirrorClose(void **mirrorHandle){
  MirrorStruct *mirstr=(MirrorStruct*)*mirrorHandle;
  printf("Closing mirror\n");
  if(mirstr!=NULL){
    pthread_mutex_lock(&mirstr->m);
    if(mirstr->paramNames!=NULL)
      free(mirstr->paramNames);
    mirstr->open=0;
    pthread_cond_signal(&mirstr->cond);//wake the thread.
    pthread_mutex_unlock(&mirstr->m);
    printf("Joining mirror worker thread\n");
    pthread_join(mirstr->threadid,NULL);//wait for worker thread to complete
    mirrordofree(mirstr);
    *mirrorHandle=NULL;
  }
  printf("Mirror closed\n");
  return 0;
}
int mirrorSend(void *mirrorHandle,int n,float *data,unsigned int frameno,double timestamp,int err  ){
  MirrorStruct *mirstr=(MirrorStruct*)mirrorHandle;
  int nclipped=0;
  int intDMCommand;
  int i;
  MirrorStructBuffered *msb;
  if(err==0 && mirstr!=NULL && mirstr->open==1){
    //printf("Sending %d values to mirror\n",n);
    pthread_mutex_lock(&mirstr->m);
    if(mirstr->swap){
      mirstr->buf=1-mirstr->buf;
      mirstr->swap=0;
    }
    msb=&mirstr->msb[mirstr->buf];
    err=mirstr->err;//get the error from the last time.  Even if there was an error, need to send new actuators, to wake up the thread... incase the error has gone away.
    //First, copy actuators.  Note, should n==mirstr->nacts.
    //Note, need to convert to uint16...
    for(i=0; i<mirstr->nacts; i++){
      intDMCommand=(int)(data[i]+0.5);
      mirstr->arr[i]=(unsigned short)intDMCommand;
      if(intDMCommand<msb->actMin[i]){
	nclipped++;
	mirstr->arr[i]=msb->actMin[i];
      }
      if(intDMCommand>msb->actMax[i]){
	nclipped++;
	mirstr->arr[i]=msb->actMax[i];
      }
    }
    //memcpy(mirstr->arr,data,sizeof(unsigned short)*mirstr->nacts);
    //Wake up the thread.
    pthread_cond_signal(&mirstr->cond);
    pthread_mutex_unlock(&mirstr->m);
    circAdd(mirstr->rtcActuatorBuf,mirstr->arr,timestamp,frameno);
  }else{
    err=1;
  }
  if(err)
    return -1;
  return nclipped;
}
/**
   This is called by a main processing thread - asynchronously with mirrorSend.
*/

int mirrorNewParam(void *mirrorHandle,paramBuf *pbuf,unsigned int frameno,arrayStruct *arr){
  MirrorStruct *mirstr=(MirrorStruct*)mirrorHandle;
  int err=0;
  //int got=0;
  int dim;
  int bufno;
  MirrorStructBuffered *msb;
  //int *indx=mirstr->bufindx;
  //MIRRORBUFFERVARIABLEINDX i;
  int j=0;
  int nfound;
  int *indx=mirstr->index;
  void **values=mirstr->values;
  char *dtype=mirstr->dtype;
  int *nbytes=mirstr->nbytes;
  if(mirstr==NULL || mirstr->open==0){
    printf("Mirror not open\n");
    return 1;
  }
  bufno=1-mirstr->buf;
  msb=&mirstr->msb[bufno];

  nfound=bufferGetIndex(pbuf,MIRRORNBUFFERVARIABLES,mirstr->paramNames,indx,values,dtype,nbytes);
  if(nfound!=MIRRORNBUFFERVARIABLES){
    for(j=0; j<MIRRORNBUFFERVARIABLES; j++){
      if(indx[j]<0){
	printf("Missing %16s\n",&mirstr->paramNames[j*16]);
	writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,"Error in mirror parameter buffer: %16s",&mirstr->paramNames[j*16]);
	err=-1;
      }
    }
  }else{
    if(dtype[MIRRORNACTS]=='i' && nbytes[MIRRORNACTS]==sizeof(int)){
      if(mirstr->nacts!=*((int*)values[MIRRORNACTS])){
	printf("Error - nacts changed - please close and reopen mirror library\n");
	err=1;
      }
    }else{
      printf("mirrornacts error\n");
      writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,"mirrornacts error");
      err=1;
    }
    if(mirstr->rtcActuatorBuf!=NULL && mirstr->rtcActuatorBuf->datasize!=mirstr->nacts*sizeof(unsigned short)){
      dim=mirstr->nacts;
      if(circReshape(mirstr->rtcActuatorBuf,1,&dim,'H')!=0){
	printf("Error reshaping rtcActuatorBuf\n");
      }
    }
    if(dtype[MIRRORACTMIN]=='H' && nbytes[MIRRORACTMIN]==sizeof(unsigned short)*mirstr->nacts){
      if(msb->actMinSize<mirstr->nacts){
	if(msb->actMin!=NULL)
	  free(msb->actMin);
	if((msb->actMin=malloc(sizeof(unsigned short)*mirstr->nacts))==NULL){
	  printf("Error allocating actMin\n");
	  msb->actMinSize=0;
	  writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,
		       "mirrorActMin malloc error");
	  err=1;
	}else{
	  msb->actMinSize=mirstr->nacts;
	}
      }
      if(msb->actMin!=NULL)
	memcpy(msb->actMin,values[MIRRORACTMIN],
	       sizeof(unsigned short)*mirstr->nacts);
    }else{
      printf("mirrorActMin error\n");
      writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,"mirrorActMin error");
      err=1;
    }
    if(dtype[MIRRORACTMAX]=='H' && nbytes[MIRRORACTMAX]==sizeof(unsigned short)*mirstr->nacts){
      if(msb->actMaxSize<mirstr->nacts){
	if(msb->actMax!=NULL)
	  free(msb->actMax);
	if((msb->actMax=malloc(sizeof(unsigned short)*mirstr->nacts))==NULL){
	  printf("Error allocating actMax\n");
	  msb->actMaxSize=0;
	  writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,
		       "mirrorActMax malloc error");
	  err=1;
	}else{
	  msb->actMaxSize=mirstr->nacts;
	}
      }
      if(msb->actMax!=NULL)
	memcpy(msb->actMax,values[MIRRORACTMAX],
	       sizeof(unsigned short)*mirstr->nacts);
    }else{
      printf("mirrorActMax error\n");
      writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,"mirrorActMax error");
      err=1;
    }
    if(dtype[MIRRORACTMAPPING]=='i' && nbytes[MIRRORACTMAPPING]==sizeof(int)*mirstr->nacts){
      if(msb->actMappingSize<mirstr->nacts){
	if(msb->actMapping!=NULL)
	  free(msb->actMapping);
	if((msb->actMapping=malloc(sizeof(int)*mirstr->nacts))==NULL){
	  printf("Error allocating actMapping\n");
	  msb->actMappingSize=0;
	  writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,"actMapping malloc error");
	  err=1;
	}else{
	  msb->actMappingSize=mirstr->nacts;
	}
      }
      if(msb->actMapping!=NULL)
	memcpy(msb->actMapping,values[MIRRORACTMAPPING],sizeof(int)*mirstr->nacts);
      msb->useActMapping=1;
    }else if(nbytes[MIRRORACTMAPPING]!=0){
      printf("actMapping error\n");
      msb->useActMapping=0;
      writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,"actMapping error");
      err=1;
    }else{
      msb->useActMapping=0;
    }
    if(dtype[MIRRORACTINIT]=='H' && nbytes[MIRRORACTINIT]%sizeof(unsigned short)==0){
      mirstr->actInit=(unsigned short*)values[MIRRORACTINIT];
      mirstr->initLen=nbytes[MIRRORACTINIT]/sizeof(unsigned short);
    }else if(nbytes[MIRRORACTINIT]!=0){
      printf("actInit error\n");
      writeErrorVA(mirstr->rtcErrorBuf,-1,frameno,"actInit error");
      err=1;
      mirstr->actInit=NULL;
      mirstr->initLen=0;
    }else{
      mirstr->actInit=NULL;
      mirstr->initLen=0;
    }
     

  }
  pthread_mutex_lock(&mirstr->m);
  mirstr->swap=1;
  pthread_mutex_unlock(&mirstr->m);
  return err;
}
