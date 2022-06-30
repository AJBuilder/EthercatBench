
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <inttypes.h>
#include <pthread.h>

#include "AKDEcatController.h"
#include "ethercat.h"






/* add ns to timespec */
void add_timespec(struct timespec *ts, int64 addtime)
{
   int64 sec, nsec;

   nsec = addtime % NSEC_PER_SEC;
   sec = (addtime - nsec) / NSEC_PER_SEC;
   ts->tv_sec += sec;
   ts->tv_nsec += nsec;
   if ( ts->tv_nsec >= NSEC_PER_SEC )
   {
      nsec = ts->tv_nsec % NSEC_PER_SEC;
      ts->tv_sec += (ts->tv_nsec - nsec) / NSEC_PER_SEC;
      ts->tv_nsec = nsec;
   }
}

/* PI calculation to get linux time synced to DC time */
 bool ec_sync(int64 reftime, uint64 cycletime , int64 *offsettime, int64 dist, int64 window, int64 *d, int64 *i)
{
   static int64 integral = 0;
   int64 delta;
   /* set linux sync point 50us later than DC sync, just as example */
   delta = (reftime - dist) % cycletime;
   if(delta > (cycletime / 2)) { delta = delta - cycletime; } // If over half-period, make neg offset behrend next period
   if(delta>0){ integral--; }
   if(delta<0){ integral++; }
   *offsettime = -(delta / 120) + (integral / 5);
   *d = delta;
   *i = integral;
   return (*d < window) && (*d > (-window)) ; // Return TRUE if error is within window
}

/* RT EtherCAT thread */
void* AKDController::ecat_Talker(void* THIS)
{
   int tempCount = 0;
   AKDController* This = (AKDController*)THIS;

   // Local variables
   struct timespec   ts;
   uint64 prevDCtime;
   uint8*   output_map_ptr;
   uint8*   input_map_ptr;
   uint8*   output_buff_ptr;
   uint8*   input_buff_ptr;

   // Buffers (To outside of thread)
   int8 wrkCounterb;
   uint inSyncCountb;
   int64 toff, sync_delta, sync_integral, diffDCtimeb;
   

   clock_gettime(CLOCK_MONOTONIC, &ts);
   ec_send_processdata();
   while(1)
   {
      wrkCounterb = ec_receive_processdata(EC_TIMEOUTRET);
      
      /* calculate next cycle start */
      add_timespec(&ts, CYCLE_NS+ toff);

      /* wait to cycle start */
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

      /* calulate toff to get linux time and DC synced */
      if(ec_sync(ec_DCtime, CYCLE_NS, &toff, SYNC_DIST, SYNC_WINDOW_NS, &sync_delta, &sync_integral)){ // If withing sync window
         if (inSyncCountb != UINT32_MAX) inSyncCountb++; // Count number of cycles in window
      }
      else inSyncCountb = 0;

      #if DEBUG_MODE
      // Try to update debug info
      if(pthread_mutex_trylock(&This->debug) == 0){
         This->gl_toff = toff;
         This->gl_delta = sync_delta;
         This->gl_integral = sync_integral;
         pthread_mutex_unlock(&This->debug);
      }
      #endif

      
      
      if(pthread_mutex_trylock(&This->control) == 0){
         // CONTROL LOCKED
         if(This->masterState == ms_shutdown) {
            pthread_mutex_unlock(&This->control);
            break;
         }
         This->inSyncCount = inSyncCountb;
         This->wrkCounter = wrkCounterb;
         This->diffDCtime = ec_DCtime - prevDCtime;
         prevDCtime = ec_DCtime;
         
         for(int slaveNum = 0 ; slaveNum < This->slaveCount ; slaveNum++){
            
            /*
            if(This->slaves[slaveNum].coeStatus & 0b01000000000000) {
               This->slaves[slaveNum].moveAck = TRUE;
               pthread_cond_signal(&This->moveSig);
            } else This->slaves[slaveNum].moveAck = FALSE;

            if(This->slaves[slaveNum].coeStatus & 0b10000000000000) {
               This->slaves[slaveNum].moveErr = TRUE;
               pthread_cond_signal(&This->moveSig);
            } else This->slaves[slaveNum].moveErr = FALSE;

            if(This->slaves[slaveNum].coeStatus & 0b00010000000000) {
               This->slaves[slaveNum].moveErr = TRUE;
               pthread_cond_signal(&This->moveSig);
            } else This->slaves[slaveNum].moveErr = FALSE;*/
            
            if(This->slaves[slaveNum].update){
               tempCount++;
               
               
               output_buff_ptr = This->slaves[slaveNum].outUserBuff;
               input_buff_ptr = This->slaves[slaveNum].inUserBuff;
               output_map_ptr = ec_slave[slaveNum].outputs;
               input_map_ptr = ec_slave[slaveNum].inputs;

               // Update outputs from user buffers to IOmap
               // Copy data from user's input to IOmap.
               memcpy(output_map_ptr, output_buff_ptr, This->slaves[slaveNum].rxPDO.bytes);

               // Update inputs from user buffers to IOmap
               memcpy(input_buff_ptr, input_map_ptr, This->slaves[slaveNum].txPDO.bytes);

               // Reset update flag
               This->slaves[slaveNum].update = FALSE;
            }
            else {
            if(This->slaves[slaveNum].mode == profPos || This->slaves[slaveNum].mode == homing) This->slaves[slaveNum].coeCtrlWord &= ~0b0010000; // Clear move bit. In homing : start_homing, In profPos : new_setpoint, In intPos : Interpolate
            }

            

            // Overwrite CoE control and status words with controllers data.
            memcpy(This->slaves[slaveNum].coeCtrlMapPtr, &This->slaves[slaveNum].coeCtrlWord, sizeof(This->slaves[slaveNum].coeCtrlWord));
            memcpy(&This->slaves[slaveNum].coeStatus, This->slaves[slaveNum].coeStatusMapPtr, sizeof(This->slaves[slaveNum].coeStatus));

            if(This->slaves[slaveNum].coeStatus != This->slaves[slaveNum].prevCoeStatus){
               This->slaves[slaveNum].statusChanged = TRUE;
               pthread_cond_signal(&This->statusUpdated);
            }
            This->slaves[slaveNum].prevCoeStatus = This->slaves[slaveNum].coeStatus;

            if(This->slaves[slaveNum].quickStop)
               This->slaves[slaveNum].coeCtrlWord &= ~0b100;
         }
         

         // Signal that IOmap has been updated (Possibly user buff)
         // Called before unlocking control so that whatever is waiting
         // on the signal has priority. (Pretty sure this is how it works...)
         pthread_cond_signal(&This->IOUpdated);
         

         pthread_mutex_unlock(&This->control);
         // CONTROL UNLOCKED
      }
      else {
         for(int slaveNum = 0; slaveNum < This->slaveCount; slaveNum++){
            if(This->slaves[slaveNum].mode == profPos || This->slaves[slaveNum].mode == homing) This->slaves[slaveNum].coeCtrlWord &= ~0b0010000; // Clear move bit. In homing : start_homing, In profPos : new_setpoint, In intPos : Interpolate
         }
         
      }
      //printf("\nMode: %i CoeStatus: 0x%x CoeCtrl: 0x%x\n", This->slaves[0].mode, This->slaves[0].coeStatus, This->slaves[0].coeCtrlWord);
      ec_send_processdata();
   }
   return nullptr;
}

void* AKDController::ecat_Controller(void* THIS)
{
   printf("ECAT: Controller Spawned\n");  

   // Local variables
   AKDController* This = (AKDController*)THIS;
   uint currentgroup = 0, noDCCount, err = 0;
   bool statusChange;
   struct timespec cycleTimeout;
   

   

   while(1)
   {
      // CONTROL LOCKED
      pthread_mutex_lock(&This->control);
      if(This->masterState == ms_shutdown){
         pthread_mutex_unlock(&This->control);  
         break;
      }
      if(This->diffDCtime == 0) noDCCount++;
      else noDCCount = 0;
      if(noDCCount > 20) This->masterState = ms_stop;

      clock_gettime(CLOCK_REALTIME, &cycleTimeout);
      add_timespec(&cycleTimeout, (int64)CNTRL_CYCLEMS * 1000 * 1000);

      err = pthread_cond_timedwait(&This->statusUpdated, &This->control, &cycleTimeout);

      for(int slaveNum = 0; slaveNum < This->slaveCount; slaveNum++){
         if (This->slaves[slaveNum].statusChanged){
            This->slaves[slaveNum].statusChanged = FALSE;

            // CANopen state machine
            switch(This->slaves[slaveNum].coeStatus & 0b01101111){
            case QUICKSTOP :
            {
               This->slaves[slaveNum].coeCurrentState = cs_QuickStop;
               if(!This->slaves[slaveNum].quickStop) {
                  This->slaves[slaveNum].coeStateTransition = cst_EnableOp;
                  This->slaves[slaveNum].coeCtrlWord |= 0b100; // Disable quickstop
               } 
               else This->slaves[slaveNum].coeStateTransition = cst_TrigQuickStop;
               break;
            }
            default :
            {
               switch(This->slaves[slaveNum].coeStatus & 0b01001111){
                  case NOTRDY2SWCH & 0b01001111: 
                  {
                     This->slaves[slaveNum].coeCurrentState = cs_NotReady;
                     This->slaves[slaveNum].coeStateTransition = cst_Shutdown; // Ready to switch
                     break;
                  }
                  case SWCHDISABLED & 0b01001111: 
                  {
                     This->slaves[slaveNum].coeCurrentState = cs_SwitchDisabled;
                     This->slaves[slaveNum].coeStateTransition = cst_Shutdown; // Ready to switch
                     This->slaves[slaveNum].coeCtrlWord = (This->slaves[slaveNum].coeCtrlWord & ~0b00001111) | 0b110; // Ready2Switch/Shutdown : 0bx---x110
                     break;
                  }
                  case RDY2SWITCH & 0b01001111:
                  { //Stop
                     This->slaves[slaveNum].coeCurrentState = cs_Ready; 
                     This->slaves[slaveNum].coeStateTransition = cst_SwitchOn; // Switch on
                     if((This->masterState == ms_enable) || (This->masterState = ms_disable)) This->slaves[slaveNum].coeCtrlWord = (This->slaves[slaveNum].coeCtrlWord & ~0b00001111) | 0b0111; // Switch-On : 0bx---x111
                     break;
                  }
                  case SWITCHEDON  & 0b01001111:
                  { // Disable
                     This->slaves[slaveNum].coeCurrentState = cs_SwitchedOn;
                     if((This->masterState == ms_stop) || (This->masterState == ms_shutdown)) {
                        This->slaves[slaveNum].coeStateTransition = cst_Shutdown; // Switch off
                        This->slaves[slaveNum].coeCtrlWord = (This->slaves[slaveNum].coeCtrlWord & ~0b00001111) | 0b0110; // Ready2Switch : 0bx---x110
                     }
                     else if(This->masterState == ms_enable){
                        This->slaves[slaveNum].coeStateTransition = cst_EnableOp; // Operation enable
                        This->slaves[slaveNum].coeCtrlWord = (This->slaves[slaveNum].coeCtrlWord & ~0b00001111) | 0b1111; // Operation Enabled : 0bx---1111
                     }
                     break;
                  }
                  case OP_ENABLED  & 0b11011111:
                  { // Enable
                     This->slaves[slaveNum].coeCurrentState = cs_OpEnabled;
                     if (This->masterState != ms_enable) {
                        This->slaves[slaveNum].coeStateTransition = cst_DisableOp; // Disable operation
                        This->slaves[slaveNum].coeCtrlWord = (This->slaves[slaveNum].coeCtrlWord & ~0b00001111) | 0b0111; // Operation Disabled : 0bx---0111
                     }
                     break;
                  }
                  case FAULT :
                  {
                     This->slaves[slaveNum].coeCurrentState = cs_Fault;
                     This->slaves[slaveNum].coeStateTransition = cst_DisableOp;
                     This->slaves[slaveNum].coeCtrlWord = (This->slaves[slaveNum].coeCtrlWord & ~0b00001111) | 0b0111; // Operation Disabled : 0bx---0111
                     break;
                  }
                  case FAULTREACT :
                  {
                     This->slaves[slaveNum].coeCurrentState = cs_FaultReaction;
                     This->slaves[slaveNum].coeStateTransition = cst_Shutdown;
                     break;
                  }
                  default :
                  {
                     This->slaves[slaveNum].coeCurrentState = cs_Unknown;
                     This->slaves[slaveNum].coeStateTransition = cst_Shutdown;
                     This->slaves[slaveNum].coeCtrlWord = (This->slaves[slaveNum].coeCtrlWord & ~0b00001111) | 0b110; // Shutddown : 0bx---x110
                     break;
                  }
               }
            }
         }
         
            #if DEBUG_MODE
            printf("\n(Slave %i)CoE State: %-24s (0x%04x)\tCoE Control: %-24s (0x%04x)\n", slaveNum + 1, This->coeStateReadable[This->slaves[slaveNum].coeCurrentState], This->slaves[slaveNum].coeStatus, This->coeCtrlReadable[This->slaves[slaveNum].coeStateTransition], This->slaves[slaveNum].coeCtrlWord);
            #endif
         }
      }

      if(This->inOP && (This->wrkCounter < This->expectedWKC) || ec_group[currentgroup].docheckstate)
         {
            /* one ore more slaves are not responding */
            ec_group[currentgroup].docheckstate = FALSE;
            ec_readstate();
            for (int slave = 1; slave <= This->slaveCount; slave++)
            {
               if ((ec_slave[slave].group == currentgroup) && (ec_slave[slave].state != EC_STATE_OPERATIONAL))
               {
                  ec_group[currentgroup].docheckstate = TRUE;
                  if (ec_slave[slave].state == (EC_STATE_SAFE_OP + EC_STATE_ERROR))
                  {
                     printf("\nECAT: (ERROR) slave %d is in SAFE_OP + ERROR, attempting ack.\n", slave);
                     ec_slave[slave].state = (EC_STATE_SAFE_OP + EC_STATE_ACK);
                     ec_writestate(slave);
                  }
                  else if(ec_slave[slave].state == EC_STATE_SAFE_OP)
                  {
                     printf("\nECAT: (WARNING) slave %d is in SAFE_OP, change to OPERATIONAL.\n", slave);
                     ec_slave[slave].state = EC_STATE_OPERATIONAL;
                     ec_writestate(slave);
                  }
                  else if(ec_slave[slave].state > EC_STATE_NONE)
                  {
                     if (ec_reconfig_slave(slave, EC_TIMEOUTMON))
                     {
                        ec_slave[slave].islost = FALSE;
                        printf("\nECAT: (MESSAGE) slave %d reconfigured\n",slave);
                     }
                  }
                  else if(!ec_slave[slave].islost)
                  {
                     /* re-check state */
                     ec_statecheck(slave, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
                     if (ec_slave[slave].state == EC_STATE_NONE)
                     {
                        ec_slave[slave].islost = TRUE;
                        printf("\nECAT: (ERROR) slave %d lost\n",slave);
                     }
                  }
               }
               if (ec_slave[slave].islost)
               {
                  if(ec_slave[slave].state == EC_STATE_NONE)
                  {
                     if (ec_recover_slave(slave, EC_TIMEOUTMON))
                     {
                        ec_slave[slave].islost = FALSE;
                        printf("\nECAT: (MESSAGE) slave %d recovered\n",slave);
                     }
                  }
                  else
                  {
                     ec_slave[slave].islost = FALSE;
                     printf("\nECAT: (MESSAGE) slave %d found\n",slave);
                  }
               }
            }
            if(!ec_group[currentgroup].docheckstate)
               printf("\nECAT: (OK) all slaves resumed OPERATIONAL.\n");
         }
        
      
      
      #if DEBUG_MODE
      pthread_mutex_lock(&This->debug);
      
      printf("\r");
      printf("WKC: %2i(%2i)\t", This->wrkCounter, This->expectedWKC);
      printf("DiffDC: %12" PRIi64 "\t", This->diffDCtime);
      printf("inSyncCount: %4i\t", This->inSyncCount);
      //printf("coeStatus: 0x%04"PRIx16"\t", This->coeStatus);
      //printf("CtrlWord: 0x%04"PRIx16"\t", This->coeCtrlWord);

      if(This->gl_toff*((This->gl_toff>0) - (This->gl_toff<0)) > (CYCLE_NS / 2)) printf("\tClock slipping! toff = %" PRIi64 "  CYCLE_NS/2 = %d\n", This->gl_toff, CYCLE_NS / 2);
      pthread_mutex_unlock(&This->debug);
      fflush(stdout);
      #endif

      pthread_mutex_unlock(&This->control);
      // CONTROL UNLOCKED

      pthread_cond_signal(&This->stateUpdated);
      
   }
   
   return nullptr;
}

bool AKDController::ecat_Init(char *ifname){

   if(this->masterState != ms_shutdown){
      printf("ECAT: Already initialized. Shutdown before restarting.\n");
      return FALSE;
   }

   if (ec_init(ifname))
   {
      printf("ECAT: ec_init on %s succeeded.\n", ifname);
      /* find and auto-config slaves */

      if( ec_config_init(FALSE) > 0 ) {
         this->masterState = ms_stop;

         // Allocate slave data
         this->slaveCount = ec_slavecount;
         this->slaves = new ecat_slave [this->slaveCount];

         // Initialize threading resources
         pthread_mutex_init(&this->control, NULL);
         pthread_mutex_init(&this->debug, NULL);
         pthread_cond_init(&this->IOUpdated, NULL);
         pthread_cond_init(&this->stateUpdated, NULL);
         pthread_cond_init(&this->statusUpdated, NULL);

         return TRUE;
      }
      else
         printf("ECAT: No slaves found!\n");
   }
   else
      printf("ECAT: No socket connection on %s. Execute as root!\n", ifname);

      return FALSE;
}

bool AKDController::ecat_Start(){

   printf("ECAT: Starting EtherCAT Master\n");

   uint32 sdoBuff;
   int sdoBuffSize;
   int sdosNotConfigured = 1;
   int PDOAssign, pdoObject, pdoCnt, entry, entryCnt, bytesTillCoE;
   struct ecat_slave::mappings_t *pdoMappings;
   

   if(this->masterState == ms_shutdown){
      printf("ECAT: Not initialized. Call ecat_Init()\n");
      return FALSE;
   }


   // Configure PDO assignments
   printf("ECAT: %d slaves found. Configuring PDO assigments...\n",slaveCount);


   for(int slaveNum = 1 ; slaveNum <= this->slaveCount ; slaveNum++){
      
      for(int i = 1; sdosNotConfigured != 0 && i <=10 ; i++){
         if(i != 1) osal_usleep(100000); // If looping, wait 100ms
         sdosNotConfigured = 0;

         //////// Write Config ////////

         // Assign PDOs

         //Disable
         sdoBuff = 0;
         ec_SDOwrite(slaveNum, rxPDOEnable, FALSE, 1, &sdoBuff, EC_TIMEOUTRXM); // Disable rxPDO
         ec_SDOwrite(slaveNum, txPDOEnable, FALSE, 1, &sdoBuff, EC_TIMEOUTRXM); // Disable txPDO

         //Write
         for(int i = 0 ; i < this->slaves[slaveNum - 1].rxPDO.numOfPDOs ; i++){
            sdoBuff = this->slaves[slaveNum - 1].rxPDO.mapObject[i];
            ec_SDOwrite(slaveNum, rxPDOAssign1, i + 1, FALSE, 2, &sdoBuff, EC_TIMEOUTRXM); // Assign rxPDO1
         }
         for(int i = 0 ; i < this->slaves[slaveNum - 1].txPDO.numOfPDOs ; i++){
            sdoBuff = this->slaves[slaveNum - 1].txPDO.mapObject[i];
            ec_SDOwrite(slaveNum, txPDOAssign1, i + 1, FALSE, 2, &sdoBuff, EC_TIMEOUTRXM); // Assign txPDO1
         }

         // Set num of pdos
         sdoBuff = this->slaves[slaveNum - 1].rxPDO.numOfPDOs;
         ec_SDOwrite(slaveNum, rxPDOEnable, FALSE, 1, &sdoBuff, EC_TIMEOUTRXM); // Enable rxPDO
         sdoBuff = this->slaves[slaveNum - 1].txPDO.numOfPDOs;
         ec_SDOwrite(slaveNum, txPDOEnable, FALSE, 1, &sdoBuff, EC_TIMEOUTRXM); // Enable txPDO

         // Configure Homing settings
         sdoBuff = DEFAULT_HMAUTOMOVE;
         ec_SDOwrite(slaveNum, HM_AUTOMOVE , FALSE, 1, &sdoBuff, EC_TIMEOUTRXM); // Disable automove

         // Configure QuickStop
         sdoBuff = DEFAULT_QSCODE;
         ec_SDOwrite(slaveNum, QSTOP_OPT , FALSE, 2, &sdoBuff, EC_TIMEOUTRXM); // Set Quickstop option

         //////// Verify Assignments ////////

         // Verify PDOs
         sdoBuffSize = 1;
         sdoBuff = 0;
         ec_SDOread(slaveNum, rxPDOEnable, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM); // Check rxPDO enabled
         if(sdoBuff != this->slaves[slaveNum - 1].rxPDO.numOfPDOs) sdosNotConfigured++;

         sdoBuffSize = 1;
         sdoBuff = 0;
         ec_SDOread(slaveNum, txPDOEnable, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM); // Check txPDO enabled
         if(sdoBuff != this->slaves[slaveNum - 1].txPDO.numOfPDOs) sdosNotConfigured++;

         for(int i = 0 ; i < this->slaves[slaveNum - 1].rxPDO.numOfPDOs ; i++){
            sdoBuffSize = 2;
            sdoBuff = 0;
            ec_SDOread(slaveNum, rxPDOAssign1, i + 1, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM); // Read rxPDO1
            if(sdoBuff != this->slaves[slaveNum - 1].rxPDO.mapObject[i]) sdosNotConfigured++;
         }
         for(int i = 0 ; i < this->slaves[slaveNum - 1].txPDO.numOfPDOs ; i++){
            sdoBuffSize = 2;
            sdoBuff = 0;
            ec_SDOread(slaveNum, txPDOAssign1, i + 1, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM); // Read txPDO1
            if(sdoBuff != this->slaves[slaveNum - 1].txPDO.mapObject[i]) sdosNotConfigured++;
         }

         // Verify Homing Config
         sdoBuffSize = 1;
         sdoBuff = 0;
         ec_SDOread(slaveNum, HM_AUTOMOVE , FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM); // Check homing automove
         if(sdoBuff != DEFAULT_HMAUTOMOVE) sdosNotConfigured++;

         // Verify QuickStop Config
         sdoBuffSize = 2;
         sdoBuff = 0;
         ec_SDOread(slaveNum, QSTOP_OPT , FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM); // Check txPDO enabled
         if(sdoBuff != DEFAULT_QSCODE) sdosNotConfigured++;

         if(sdosNotConfigured != 0) printf("\rECAT: PDO assignments failed to configure %2d time(s). Retrying...  ", i);
      }
      if(sdosNotConfigured == 0) printf("PDO's configured!\n");
      else {
         printf("PDO's failed to configure.\n");
         return FALSE;
      }

      
      
   }


   ecx_context.manualstatechange = 1;
   ec_config_map(&this->IOmap);
   
   // Compare user buffer size to actual size. Also calculate larges???
   for(int slaveNum = 1 ; slaveNum <= this->slaveCount ; slaveNum++){
      if(slaves[slaveNum-1].totalBytes != (ec_slave[slaveNum].Obytes + ec_slave[slaveNum].Ibytes)){
         printf("ECAT: Input struct is of incorrect size. Is %i. Needs to be %i", slaves[slaveNum-1].totalBytes, (ec_slave[slaveNum].Obytes + ec_slave[slaveNum].Ibytes));
         return FALSE;
      }

      slaves[slaveNum-1].coeCtrlOffset= -1;
      slaves[slaveNum-1].coeStatusOffset = -1;

      pdoMappings = &slaves[slaveNum - 1].rxPDO;
      PDOAssign = rxPDOAssign1;
      while(1){

         bytesTillCoE = 0;
         pdoMappings->bytes = 0;
         
         //Read PDO assign subindex 0 ( = number of PDO's)
         sdoBuffSize = 2; sdoBuff = 0;         
         ec_SDOread(slaveNum, PDOAssign, 0x00, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM);

         if (sdoBuff > 0)
         {
            // Check to see if read value matches user's specified
            if(sdoBuff != pdoMappings->numOfPDOs){
               printf("ECAT: Number of user specified PDOs(%i) does not match the number of read PDOs(%i). Aborting Start()\n", pdoCnt, sdoBuff);
               return FALSE;
            }

            // Read all PDO assignments
            for (int idxloop = 1; idxloop <= pdoMappings->numOfPDOs; idxloop++)
            {
               // Read PDO map object
               sdoBuffSize = 2; sdoBuff = 0;
               ec_SDOread(slaveNum, PDOAssign, (uint8)idxloop, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM);
               pdoObject = sdoBuff;

               
               if (pdoObject > 0)
               {
                  // Read number of entries mapped
                  sdoBuffSize = 2; sdoBuff = 0;
                  ec_SDOread(slaveNum, pdoObject, 0x00, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM);
                  entryCnt = sdoBuff;

                  // Read each entries size
                  for (int subidxloop = 1; subidxloop <= entryCnt; subidxloop++)
                  {
                     // Read object
                     sdoBuffSize = 4; sdoBuff = 0;
                     ec_SDOread(slaveNum, pdoObject, (uint8)subidxloop, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM);
                     
                     //extract bitlength of SDO
                     if(PDOAssign == rxPDOAssign1){
                        if(((sdoBuff & 0xFFFF0000) >> 16) == COControl){
                           if(slaves[slaveNum-1].coeCtrlOffset!= -1){
                              printf("ECAT: Found multiple CoE control word mappings when reading Slave %i's mapping. Aborting Start().\n", slaveNum);
                              return FALSE;
                           }
                           slaves[slaveNum-1].coeCtrlOffset= bytesTillCoE;
                        }
                        else{
                           bytesTillCoE += (sdoBuff & 0xFF) / 8;
                        }
                     }
                     else{
                        if(((sdoBuff & 0xFFFF0000) >> 16) == COStatus){
                           if(slaves[slaveNum-1].coeStatusOffset != -1){
                              printf("ECAT: Found multiple CoE status mappings when reading Slave %i's mapping. Aborting Start().\n", slaveNum);
                              return FALSE;
                           }  
                           slaves[slaveNum-1].coeStatusOffset = bytesTillCoE;
                        }
                        else{
                           bytesTillCoE += (sdoBuff & 0xFF) / 8;
                        }
                     }
                     pdoMappings->bytes += (sdoBuff & 0xFF) / 8;
                  }
               }
               else{
                  printf("ECAT: Tried to read PDO mapping. Slave %i, assign 0x%x, PDO %i is zero? Aborting Start().\n", slaveNum, PDOAssign, idxloop);
                  return FALSE;
               }
            }
         }
         else{
            printf("ECAT: Tried to read PDO assignments. Slave %i, assign 0x%x is disabled? Aborting Start().\n", slaveNum, PDOAssign);
            return FALSE;
         }

         // Repeat but with txPDOs
         pdoMappings = &slaves[slaveNum - 1].txPDO;
         if(PDOAssign == txPDOAssign1) break;
         PDOAssign = txPDOAssign1;
         
      }
   
      // Finish Populating User Buffer PDO maps
      this->slaves[slaveNum - 1].inUserBuff = this->slaves[slaveNum - 1].outUserBuff + ec_slave[slaveNum].Obytes;

      // Find where the AKD CoE control word is in IOmap
      this->slaves[slaveNum - 1].coeCtrlMapPtr = ec_slave[slaveNum].outputs + this->slaves[slaveNum - 1].coeCtrlOffset;

      // Find where the AKD CoE status word is in IOmap
      this->slaves[slaveNum - 1].coeStatusMapPtr = ec_slave[slaveNum].inputs + this->slaves[slaveNum - 1].coeStatusOffset;

      
   }


   

   // Config DC (In PREOP)
   printf("ECAT: Configuring DC. Should be in PRE-OP. ");
   printf(ec_statecheck(0, EC_STATE_PRE_OP,  EC_TIMEOUTSTATE * 4) == EC_STATE_PRE_OP ? "State: PRE_OP\n" : "ERR! NOT IN PRE_OP!\n") ;
   printf(ec_configdc() ? "ECAT: Found slaves with DC.\n" : "ECAT: Did not find slaves with DC.\n") ;
   ec_dcsync0(1, FALSE, CYCLE_NS, 0);
   
   
   
   // Syncing (In SAFEOP)
   printf("ECAT: Slaves mapped, state to SAFE_OP... ");
   ec_slave[0].state = EC_STATE_SAFE_OP;
   ec_writestate(0);
   printf(ec_statecheck(0, EC_STATE_SAFE_OP,  EC_TIMEOUTSTATE * 4) == EC_STATE_SAFE_OP ? "State: SAFE_OP\n" : "ERR! NOT IN SAFE_OP!\n") ;
   this->expectedWKC = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;
   printf("ECAT: Calculated workcounter %d\n", this->expectedWKC);
   
   /*for(int i = 0 ; i < 100 ; i++){ // Send 10,000 cycles of process data to tune other slaves
      ec_send_processdata();
      wkc = ec_receive_processdata(EC_TIMEOUTRET);
      printf("\rSending 100 process cycles: %5d WKC : %d", i, wkc);
   }*/

   

   /* Enable talker to handle slave PDOs in OP */
   pthread_create(&this->talker, NULL, &AKDController::ecat_Talker, this);
   pthread_create(&this->controller, NULL, &ecat_Controller, this);
   printf( "ECAT: Talker started! Synccount needs to reach : %" PRIi64 "\n", (uint64)SYNC_AQTIME_NS / CYCLE_NS);
   pthread_mutex_lock(&this->control);
   while(this->inSyncCount < (uint64)(SYNC_AQTIME_NS / CYCLE_NS)){
      pthread_mutex_unlock(&this->control);
      osal_usleep(100000); // 100ms
      pthread_mutex_lock(&this->control);
   }
   pthread_mutex_unlock(&this->control);
   printf("\nECAT: Master within sync window. Enabling sync0 generation!\n");
   ec_dcsync0(1, TRUE, CYCLE_NS, 0); //Enable sync0 generation


   // Go into OPERATIONAL
   printf("\nECAT: Requesting operational state for all slaves\n");
   ec_slave[0].state = EC_STATE_OPERATIONAL;
   ec_send_processdata();  // Send one valid data to prep slaves for OP state
   ec_receive_processdata(EC_TIMEOUTRET);
   ec_writestate(0);
   ec_statecheck(0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE); //wait for OP


   // Return results
   if (ec_slave[0].state == EC_STATE_OPERATIONAL )
   {
      printf("\nECAT: Operational state reached for all slaves!\n");
      this->inOP = TRUE;
      return TRUE;
   }
   else
   {
      printf("\nECAT: Not all slaves reached operational state.\n");
      ec_readstate();
      for(int i = 1; i <= this->slaveCount ; i++)
      {
         ec_dcsync0(i+1, FALSE, 0, 0); // SYNC0,1
         if(ec_slave[i].state != EC_STATE_OPERATIONAL)
         {
            printf("ECAT: Slave %d State=0x%2.2x StatusCode=0x%4.4x : %s\n",
                  i, ec_slave[i].state, ec_slave[i].ALstatuscode, ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
         }
      }
   }

   

   return FALSE;
}

// Blocks for timeout time
int AKDController::Update(uint slave, bool move, int timeout_ms){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }

   int err = 0, slaveNum;
   struct timespec timeout;
   bool allUpdated = FALSE;

   clock_gettime(CLOCK_REALTIME, &timeout);
   add_timespec(&timeout, (int64)timeout_ms * 1000 * 1000);

   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{ // Set update flag (And possibly bit 4)

      this->slaves[slaveNum].update = TRUE;
      if(move && (slaves[slaveNum].mode == homing || slaves[slaveNum].mode == profPos || slaves[slaveNum].mode == intPos) ) 
         this->slaves[slaveNum].coeCtrlWord |= 0b0010000; // Should only be used if in homing, interpolated or profPos mode.

      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));

   allUpdated = FALSE;
   while(!allUpdated && err == 0){ // Check and make sure all slave update flags were recognized.

      if(timeout_ms != 0) err = pthread_cond_timedwait(&this->IOUpdated, &this->control, &timeout); //Wait for talker thread to confirm update to IOmap
      else pthread_cond_wait(&this->IOUpdated, &this->control);

      allUpdated = TRUE;
      if(slave != 0) slaveNum = slave - 1;
      else slaveNum = 0;
      do{ 
         if (this->slaves[slaveNum].update == TRUE){
            allUpdated = FALSE;
            break;
         }

         slaveNum++;
      }while((slaveNum < this->slaveCount) && (slave == 0));

   }

   allUpdated = FALSE;
   while(!allUpdated && err == 0 && move){ // Check to see if update was acknowledged or error

      
      if(timeout_ms != 0) err = pthread_cond_timedwait(&this->IOUpdated, &this->control, &timeout); //Wait for talker thread to confirm update to move status.
      else pthread_cond_wait(&this->IOUpdated, &this->control);

      allUpdated = TRUE;
      if(slave != 0) slaveNum = slave - 1;
      else slaveNum = 0;
      do{
         if (!(this->slaves[slaveNum].coeStatus & (1 << 12)) && this->slaves[slaveNum].mode != intPos){
            allUpdated = FALSE;
            break;
         }
         if(this->slaves[slaveNum].coeStatus & (1 << 13)) {
            return AKD_MOVEERR;
            break;
         }

         slaveNum++;
      }while((slaveNum < this->slaveCount) && (slave == 0));
         
   }
   
   pthread_mutex_unlock(&this->control);
   return err; // Return err
}
 
 // Blocks for max of 1 sec;
bool AKDController::State(ecat_masterStates reqState){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }

   struct timespec timeout;
   uint sdoBuffSize, sdoBuff;
   int err = 0;
   bool allStatesChanged = FALSE;
   ecat_coeStates waitingFor;
   ecat_masterStates oldState;

   if(this->masterState != reqState){
         if(reqState != ms_shutdown){

            oldState = this->masterState;
            this->masterState = reqState;

            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 1; // 1 sec timeout
            
            switch(reqState){ //Wait for talker thread to confirm update
                  case ms_stop :
                     waitingFor = cs_Ready;
                  break;
                  case ms_disable :
                     waitingFor = cs_SwitchedOn;
                  break;
                  case ms_enable :
                     waitingFor = cs_OpEnabled;
                  break;
                  default :
                     err = -1;
                  break;
            }

            while(!allStatesChanged && err == 0){
               allStatesChanged = TRUE;
               for(int slaveNum = 0 ; slaveNum < this->slaveCount ; slaveNum++){
                  if (this->slaves[slaveNum].coeCurrentState != waitingFor){
                     
                     if(this->slaves[slaveNum].coeCurrentState == cs_Fault){
                        pthread_mutex_unlock(&this->control);
                        this->masterState = oldState; // Reset state
                        return FALSE; // If a slave is in fault, return 
                     }

                     allStatesChanged = FALSE;
                     break;
                  }
               }
               err = pthread_cond_timedwait(&this->stateUpdated, &this->control, &timeout);
            }
         }
        else{// Shutdown threads
            this->masterState = ms_shutdown;
            pthread_mutex_unlock(&this->control);
            pthread_join(this->talker, NULL);
            pthread_join(this->controller, NULL);

            printf("\nECAT: Requesting init state for all slaves\n");
            ec_slave[0].state = EC_STATE_INIT;
            /* request INIT state for all slaves */
            ec_writestate(0);

            printf("ECAT: End simple test, close socket\n");

            pthread_mutex_destroy(&this->control);
            pthread_mutex_destroy(&this->debug);

            pthread_cond_destroy(&this->IOUpdated);
            pthread_cond_destroy(&this->stateUpdated);
            //pthread_cond_destroy(&this->moveSig);

            /* stop SOEM, close socket */
            ec_close();
        }
   }

   pthread_mutex_unlock(&this->control);
   return err == 0;

}

bool AKDController::Enable(){
   return AKDController::State(ms_enable);
}

bool AKDController::Disable(){
   return AKDController::State(ms_disable);
}

bool AKDController::Stop(){
   return AKDController::State(ms_stop);
}

bool AKDController::Shutdown(){
   return AKDController::State(ms_shutdown);
}

// Blocking until quick stop engaged
bool AKDController::QuickStop(uint slave, bool enableQuickStop){ 

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }

   int slaveNum;
   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{

      if(enableQuickStop)
         this->slaves[slaveNum].quickStop = TRUE;
      else
         this->slaves[slaveNum].quickStop = FALSE;

      slaveNum++;
   }while((slaveNum < this->slaveCount) && (slave == 0));

   pthread_mutex_unlock(&this->control);
   return TRUE;
}

// Can block for 1 sec
bool AKDController::setOpMode(uint slave, ecat_OpModes reqMode){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }
   pthread_mutex_unlock(&this->control);

   int sdoBuffSize, sdoBuff, slaveNum;
   struct timespec timeout, curtime;
   clock_gettime(CLOCK_MONOTONIC, &timeout);
   timeout.tv_sec += 1;

   
   if(AKDController::State(ms_disable)){ // Don't assign if not disabled 
      
      pthread_mutex_lock(&this->control);
      
      if(slave != 0) slaveNum = slave;
      else slaveNum = 1;
      do{

         if(this->slaves[slaveNum].mode != reqMode){  
   
            do{
               sdoBuffSize = 1;
               sdoBuff = reqMode;
               ec_SDOwrite(slaveNum, REQOPMODE, FALSE, sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM); // Assign Operational Mode

               sdoBuffSize = 1; // Check if drive has acknowledged
               sdoBuff = 0;
               ec_SDOread(slaveNum, ACTOPMODE, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTRXM);

               clock_gettime(CLOCK_MONOTONIC, &curtime);
               if(curtime.tv_sec > timeout.tv_sec){
                  pthread_mutex_unlock(&this->control);
                  return FALSE;
               }

            } while(sdoBuff != reqMode && curtime.tv_sec < timeout.tv_sec);

            this->slaves[slaveNum-1].mode = reqMode;
         }

         slaveNum++;
      }while((slaveNum <= this->slaveCount) && (slave == 0));

      pthread_mutex_unlock(&this->control);
      AKDController::State(ms_enable);
   }

   
   
   return TRUE;
}

bool AKDController::confProfPos(uint slave, bool moveImmediate, bool moveRelative){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }

   int slaveNum;
   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{
      if(this->slaves[slaveNum].mode != profPos){
         pthread_mutex_unlock(&this->control);
         return FALSE;
      }

      if(moveImmediate) this->slaves[slaveNum].coeCtrlWord |= 0b00100000;
      else this->slaves[slaveNum].coeCtrlWord &= ~0b00100000;

      if(moveRelative) this->slaves[slaveNum].coeCtrlWord |= 0b01000000;
      else this->slaves[slaveNum].coeCtrlWord &= ~0b01000000;

      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));
   pthread_mutex_unlock(&this->control);
   
   return TRUE;
}

bool AKDController::confUnits(uint slave, uint32 motorRev, uint32 shaftRev){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }
   pthread_mutex_unlock(&this->control);

   int slaveNum, sdoBuff, sdoBuffSize;
   
   if(slave != 0) slaveNum = slave;
   else slaveNum = 1;
   do{ 
      sdoBuff = motorRev;
      ec_SDOwrite(slaveNum, MOTOR_RATIO , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); // Set acceleration for profile position mode
      sdoBuff = shaftRev;
      ec_SDOwrite(slaveNum, SHAFT_RATIO , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); // Set decceleration for profile position mode
      
      sdoBuffSize = 4;
      ec_SDOread(slaveNum, MOTOR_RATIO, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTTXM);
      if(sdoBuff != motorRev) return FALSE;
      sdoBuffSize = 4;
      ec_SDOread(slaveNum, SHAFT_RATIO, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTTXM);
      if(sdoBuff != shaftRev) return FALSE;
   
      slaveNum++;
   } while((slaveNum <= this->slaveCount) && (slave == 0));

   return TRUE;
}

bool AKDController::confMotionTask(uint slave, uint vel, uint acc, uint dec){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }
   pthread_mutex_unlock(&this->control);

   int slaveNum, sdoBuff, sdoBuffSize;
   if(slave != 0) slaveNum = slave;
   else slaveNum = 1;
   do{ 
      sdoBuff = acc;
      ec_SDOwrite(slaveNum, MT_ACC , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); // Set acceleration for profile position mode
      sdoBuff = dec;
      ec_SDOwrite(slaveNum, MT_DEC , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); // Set decceleration for profile position mode
      sdoBuff = vel;
      ec_SDOwrite(slaveNum, MT_V, FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); // Set velocity for profile position mode

      sdoBuffSize = 4;
      ec_SDOread(slaveNum, MT_ACC, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTTXM);
      if(sdoBuff != acc) return FALSE;
      sdoBuffSize = 4;
      ec_SDOread(slaveNum, MT_DEC, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTTXM);
      if(sdoBuff != dec)  return FALSE;
      sdoBuffSize = 4;
      ec_SDOread(slaveNum, MT_V, FALSE, &sdoBuffSize, &sdoBuff, EC_TIMEOUTTXM);
      if(sdoBuff != vel)  return FALSE;
   
      slaveNum++;
   } while((slaveNum <= this->slaveCount) && (slave == 0));

   return TRUE;
}

void AKDController::confSlavePDOs(uint slave, void* usrControl, int bufferSize, uint16 rxPDO1, uint16 rxPDO2, uint16 rxPDO3, uint16 rxPDO4, uint16 txPDO1, uint16 txPDO2, uint16 txPDO3, uint16 txPDO4){
   int slaveNum, rx = 0, tx = 0;
   
   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return;
   }

   if(rxPDO1 != 0) rx++;
   if(rxPDO2 != 0) rx++;
   if(rxPDO3 != 0) rx++;
   if(rxPDO4 != 0) rx++;

   if(txPDO1 != 0) tx++;
   if(txPDO2 != 0) tx++;
   if(txPDO3 != 0) tx++;
   if(txPDO4 != 0) tx++;

   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{

      slaves[slaveNum].rxPDO.numOfPDOs = rx;
      slaves[slaveNum].txPDO.numOfPDOs = tx;

      slaves[slaveNum].rxPDO.mapObject[0] = rxPDO1;
      slaves[slaveNum].rxPDO.mapObject[1] = rxPDO2;
      slaves[slaveNum].rxPDO.mapObject[2] = rxPDO3;
      slaves[slaveNum].rxPDO.mapObject[3] = rxPDO4;
      
      slaves[slaveNum].txPDO.mapObject[0] = txPDO1;
      slaves[slaveNum].txPDO.mapObject[1] = txPDO2;
      slaves[slaveNum].txPDO.mapObject[2] = txPDO3;
      slaves[slaveNum].txPDO.mapObject[3] = txPDO4;

      slaves[slaveNum].totalBytes = bufferSize;

      slaves[slaveNum].outUserBuff = (uint8*)usrControl;
      
      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));
   

   pthread_mutex_unlock(&this->control);
}

void AKDController::confDigOutputs(uint slave, uint32 bitmask, uint8 out1Mode, uint8 out2Mode){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return;
   }

   int slaveNum;

   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{

      slaves[slaveNum].digOutBitmask = bitmask;
      slaves[slaveNum].digOut1Mode = out1Mode;
      slaves[slaveNum].digOut2Mode = out2Mode;
      
      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));

   pthread_mutex_unlock(&this->control);

}

// Can block for timeout_ms
int AKDController::Home(uint slave, int HOME_MODE, int HOME_DIR, int speed, int acceleration, int HOME_DIST, int HOME_P, int timeout_ms){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }

   uint sdoBuff, err = 0, slaveNum;
   int subTimeout;
   ecat_OpModes prevMode[slaveCount];
   struct timespec timeout1, timeout2;

   clock_gettime(CLOCK_REALTIME, &timeout1);
   add_timespec(&timeout1, (int64)timeout_ms * 1000 * 1000);
   

   if(slave != 0) slaveNum = slave;
   else slaveNum = 1;
   do{ 
      prevMode[slaveNum-1] = this->slaves[slaveNum-1].mode;

      // Configure Homing parameters.
      sdoBuff = HOME_MODE;
      ec_SDOwrite(slaveNum, HM_MODE , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM);
      
      // HOME.DIR
      sdoBuff = HOME_DIR;
      ec_SDOwrite(slaveNum, HM_DIR , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); 

      // HOME.V
      sdoBuff = speed;
      ec_SDOwrite(slaveNum, HM_V , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); 

      // HOME.ACC/HOME.DEC
      sdoBuff = acceleration;
      ec_SDOwrite(slaveNum, HMACCEL , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM);

      // HOME.DIST
      sdoBuff = HOME_DIST;
      ec_SDOwrite(slaveNum, HM_DIST , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM);

      // HOME.P
      sdoBuff = HOME_P;
      ec_SDOwrite(slaveNum, HM_P , FALSE, 4, &sdoBuff, EC_TIMEOUTRXM); 

      slaveNum++;
   } while((slaveNum <= this->slaveCount) && (slave == 0));

   
   pthread_mutex_unlock(&this->control);

   if(!setOpMode(slave, homing)) {
      #if DEBUG_MODE
         printf("\nECAT: [Home] Failed to set homing mode.\n");
      #endif
      return FALSE;
   }

   if(timeout_ms != 0){
      clock_gettime(CLOCK_REALTIME, &timeout2);
      timeout2.tv_sec = timeout1.tv_sec - timeout2.tv_sec;
      timeout2.tv_nsec = timeout1.tv_nsec - timeout2.tv_nsec;
      subTimeout = (timeout2.tv_sec * 1000) + (timeout2.tv_nsec / 1000000);
   }
   else{
      subTimeout = 0;
   }
   
   if(Update(slave, TRUE, subTimeout) != 0){
      #if DEBUG_MODE
         printf("\nECAT: [Home] Update() failed.\n");
      #endif
      return FALSE;
   }

   if(timeout_ms != 0){
      clock_gettime(CLOCK_REALTIME, &timeout2);
      timeout2.tv_sec = timeout1.tv_sec - timeout2.tv_sec;
      timeout2.tv_nsec = timeout1.tv_nsec - timeout2.tv_nsec;
      subTimeout = (timeout2.tv_sec * 1000) + (timeout2.tv_nsec / 1000000);
   }
   else{
      subTimeout = 0;
   }
   if(!waitForTarget(slave, subTimeout)) return FALSE;

   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{ 
      if(!setOpMode(slaveNum+1, prevMode[slaveNum])) {
         #if DEBUG_MODE
            printf("\nECAT: [Home] Failed to revert opmode.\n");
         #endif
         return FALSE;
      }
      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));

   return TRUE;
}

bool AKDController::waitForTarget(uint slave, uint timeout_ms){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }
   int slaveNum, err = 0;
   bool allFin = FALSE;
   struct timespec timeout;

   clock_gettime(CLOCK_REALTIME, &timeout);
   add_timespec(&timeout, (int64)timeout_ms * 1000 * 1000);

   
   while(!allFin && err == 0){ // Check to see if update was acknowledged or error

      if(timeout_ms != 0) err = pthread_cond_timedwait(&this->IOUpdated, &this->control, &timeout); //Wait for talker thread to confirm update to move status.
      else err = pthread_cond_wait(&this->IOUpdated, &this->control);

      allFin = TRUE;
      if(slave != 0) slaveNum = slave - 1;
      else slaveNum = 0;
      do{
         if ( !(this->slaves[slaveNum].coeStatus & (1 << 10)) && ((this->slaves[slaveNum].mode == profPos) || (this->slaves[slaveNum].mode == homing) || (this->slaves[slaveNum].mode == profVel))){
            allFin = FALSE;
            break;
         }
         if ( (this->slaves[slaveNum].coeStatus & (1 << 11)) && ((this->slaves[slaveNum].mode == profPos) || (this->slaves[slaveNum].mode == homing) || (this->slaves[slaveNum].mode == profVel))){
            err = -1; // If internal limit active, return -1;
            break;
         }

         slaveNum++;
      }while((slaveNum < this->slaveCount) && (slave == 0));
   }

   pthread_mutex_unlock(&this->control);
   return err == 0;
}

// Returns TRUE if fault
bool AKDController::readFault(uint slave){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }

   int slaveNum;

   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{ 

      if(this->slaves[slave - 1].coeCurrentState == cs_Fault){
         pthread_mutex_unlock(&this->control);
         return TRUE;
      }
   
      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));

   pthread_mutex_unlock(&this->control);
   return FALSE;
}

bool AKDController::clearFault(uint slave, bool persistClear){

   pthread_mutex_lock(&this->control);
   if(this->masterState == ms_shutdown) {
      pthread_mutex_unlock(&this->control);
      return FALSE;
   }


   int err = 0, slaveNum;
   bool noFaults;
   struct timespec timeout;
   clock_gettime(CLOCK_MONOTONIC, &timeout);
   timeout.tv_sec += 1;

   // Loop through all slaves and set clear fault bit
   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{
      this->slaves[slaveNum].coeCtrlWord |= 0b10000000; // Clear Fault : 0b1---xxxx
      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));

   // Loop through all slaves and check that all are out of fault.
   while(!noFaults && err == 0) {
      noFaults = TRUE;

      err = pthread_cond_timedwait(&this->stateUpdated, &this->control, &timeout);

      if(slave != 0) slaveNum = slave - 1;
      else slaveNum = 0;
      do{
         if(this->slaves[slaveNum].coeCurrentState == cs_Fault){
            noFaults = FALSE;
            break;
         }
         slaveNum++;
      } while((slaveNum < this->slaveCount) && (slave == 0));
   }

   if(slave != 0) slaveNum = slave - 1;
   else slaveNum = 0;
   do{
      if(!persistClear) this->slaves[slave - 1].coeCtrlWord &= ~0b10000000; // Clear Fault : 0b1---xxxx
      slaveNum++;
   } while((slaveNum < this->slaveCount) && (slave == 0));

   pthread_mutex_unlock(&this->control);

   return err == 0;
}
