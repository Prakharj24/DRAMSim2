/*********************************************************************************
*  Copyright (c) 2010-2011, Elliott Cooper-Balis
*                             Paul Rosenfeld
*                             Bruce Jacob
*                             University of Maryland 
*                             dramninjas [at] gmail [dot] com
*  All rights reserved.
*  
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions are met:
*  
*     * Redistributions of source code must retain the above copyright notice,
*        this list of conditions and the following disclaimer.
*  
*     * Redistributions in binary form must reproduce the above copyright notice,
*        this list of conditions and the following disclaimer in the documentation
*        and/or other materials provided with the distribution.
*  
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
*  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
*  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
*  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
*  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
*  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
*  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
*  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
*  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*********************************************************************************/



//MemoryController.cpp
//
//Class file for memory controller object
//

#include "MemoryController.h"
#include "MemorySystem.h"
#include "AddressMapping.h"
#include <assert.h>
#define SEQUENTIAL(rank,bank) (rank*NUM_BANKS)+bank
#define RB_MAX (max(NUM_RANKS, NUM_BANKS) + 1)

/* Power computations are localized to MemoryController.cpp */ 
extern unsigned IDD0;
extern unsigned IDD1;
extern unsigned IDD2P;
extern unsigned IDD2Q;
extern unsigned IDD2N;
extern unsigned IDD3Pf;
extern unsigned IDD3Ps;
extern unsigned IDD3N;
extern unsigned IDD4W;
extern unsigned IDD4R;
extern unsigned IDD5;
extern unsigned IDD6;
extern unsigned IDD6L;
extern unsigned IDD7;
extern float Vdd; 

using namespace DRAMSim;

MemoryController::MemoryController(MemorySystem *parent, CSVWriter &csvOut_, ostream &dramsim_log_) :
		dramsim_log(dramsim_log_),
		bankStates(NUM_RANKS, vector<BankState>(NUM_BANKS, dramsim_log)),
		commandQueue(bankStates, dramsim_log_),
		poppedBusPacket(NULL),
		csvOut(csvOut_),
		totalTransactions(0),
		refreshRank(0)
{
	//get handle on parent
	parentMemorySystem = parent;


	//bus related fields
	outgoingCmdPacket = NULL;
	outgoingDataPacket = NULL;
	dataCyclesLeft = 0;
	cmdCyclesLeft = 0;

	//set here to avoid compile errors
	currentClockCycle = 0;

	//reserve memory for vectors
	transactionQueue.reserve(TRANS_QUEUE_DEPTH);
	powerDown = vector<bool>(NUM_RANKS,false);
	grandTotalBankAccesses = vector<long long int>(NUM_RANKS*NUM_BANKS,0);


	writeDataCountdown.reserve(NUM_RANKS);
	writeDataToSend.reserve(NUM_RANKS);
	refreshCountdown.reserve(NUM_RANKS);

	//Power related packets
	backgroundEnergy = vector <uint64_t >(NUM_RANKS,0);
	burstEnergy = vector <uint64_t> (NUM_RANKS,0);
	actpreEnergy = vector <uint64_t> (NUM_RANKS,0);
	refreshEnergy = vector <uint64_t> (NUM_RANKS,0);


	totalLatency  = vector<double>(NUM_CPU,0.0);
	totalLatencyPref = vector<double>(NUM_CPU,0.0);


        for(int i=0;i<NUM_CPU;i++){
            totalReads[i] = 0;
            totalPrefReads[i] = 0;
            totalWrites[i] = 0;
        }

	//staggers when each rank is due for a refresh
	for (size_t i=0;i<NUM_RANKS;i++)
	{
		refreshCountdown.push_back((int)((REFRESH_PERIOD/tCK)/NUM_RANKS)*(i+1));
	}

	// SecMC-NI related initialization
	
	epochStart = 0;
	dispatchTick = 0;
	rankIndx = 0;
	bankIndx = 1;
	for(int i=0; i<NUM_CPU;i++){
		rankQ[i].reserve(NUM_RANKS);
		for(size_t j=0;j<NUM_RANKS;j++){
			rankQ[i][j].reserve(TRANS_QUEUE_DEPTH);
		}
	}

	uint64_t M = max(NUM_RANKS, NUM_BANKS) + 1;
	for(int i=0;i<3;i++){
		for(int j=0;j<4;j++){
			sch[i][j] = M;
		}
	}

	turn = 0;
}	

//get a bus packet from either data or cmd bus
void MemoryController::receiveFromBus(BusPacket *bpacket)
{
	if (bpacket->busPacketType != DATA)
	{
		ERROR("== Error - Memory Controller received a non-DATA bus packet from rank");
		bpacket->print();
		exit(0);
	}

	if (DEBUG_BUS)
	{
		PRINTN(" -- MC Receiving From Data Bus : ");
		bpacket->print();
	}

	//add to return read data queue
	returnTransaction.push_back(new Transaction(RETURN_DATA, bpacket->physicalAddress, bpacket->data));

	// this delete statement saves a mindboggling amount of memory
	delete(bpacket);
}

//sends read data back to the CPU
void MemoryController::returnReadData(const Transaction *trans)
{
	if (parentMemorySystem->ReturnReadData!=NULL)
	{
		(*parentMemorySystem->ReturnReadData)(parentMemorySystem->systemID, trans->address, currentClockCycle);
	}
}

//gives the memory controller a handle on the rank objects
void MemoryController::attachRanks(vector<Rank *> *ranks)
{
	this->ranks = ranks;
}

//memory controller update
void MemoryController::update()
{

	//PRINT(" ------------------------- [" << currentClockCycle << "] -------------------------");

	//update bank states
	for (size_t i=0;i<NUM_RANKS;i++)
	{
		for (size_t j=0;j<NUM_BANKS;j++)
		{
			if (bankStates[i][j].stateChangeCountdown>0)
			{
				//decrement counters
				bankStates[i][j].stateChangeCountdown--;

				//if counter has reached 0, change state
				if (bankStates[i][j].stateChangeCountdown == 0)
				{
					switch (bankStates[i][j].lastCommand)
					{
						//only these commands have an implicit state change
					case WRITE_P:
					case READ_P:
						bankStates[i][j].currentBankState = Precharging;
						bankStates[i][j].lastCommand = PRECHARGE;
						bankStates[i][j].stateChangeCountdown = tRP;
						break;

					case REFRESH:
					case PRECHARGE:
						bankStates[i][j].currentBankState = Idle;
						break;
					default:
						break;
					}
				}
			}
		}
	}


	//check for outgoing command packets and handle countdowns
	if (outgoingCmdPacket != NULL)
	{
		cmdCyclesLeft--;
		if (cmdCyclesLeft == 0) //packet is ready to be received by rank
		{
			(*ranks)[outgoingCmdPacket->rank]->receiveFromBus(outgoingCmdPacket);
			outgoingCmdPacket = NULL;
		}
	}

	//check for outgoing data packets and handle countdowns
	if (outgoingDataPacket != NULL)
	{
		dataCyclesLeft--;
		if (dataCyclesLeft == 0)
		{
			//inform upper levels that a write is done
			if (parentMemorySystem->WriteDataDone!=NULL)
			{
				(*parentMemorySystem->WriteDataDone)(parentMemorySystem->systemID,outgoingDataPacket->physicalAddress, currentClockCycle);
			}

			(*ranks)[outgoingDataPacket->rank]->receiveFromBus(outgoingDataPacket);
			outgoingDataPacket=NULL;
		}
	}


	//if any outstanding write data needs to be sent
	//and the appropriate amount of time has passed (WL)
	//then send data on bus
	//
	//write data held in fifo vector along with countdowns
	if (writeDataCountdown.size() > 0)
	{
		for (size_t i=0;i<writeDataCountdown.size();i++)
		{
			writeDataCountdown[i]--;
		}

		if (writeDataCountdown[0]==0)
		{
			//send to bus and print debug stuff
			if (DEBUG_BUS)
			{
				PRINTN(" -- MC Issuing On Data Bus    : ");
				writeDataToSend[0]->print();
			}

			// queue up the packet to be sent
			if (outgoingDataPacket != NULL)
			{
				ERROR("== Error - Data Bus Collision");
				exit(-1);
			}

			outgoingDataPacket = writeDataToSend[0];
			dataCyclesLeft = BL/2;

			totalTransactions++;
			//totalWritesPerBank[][SEQUENTIAL(writeDataToSend[0]->rank,writeDataToSend[0]->bank)]++;

			writeDataCountdown.erase(writeDataCountdown.begin());
			writeDataToSend.erase(writeDataToSend.begin());
		}
	}

	//if its time for a refresh issue a refresh
	// else pop from command queue if it's not empty
	if (refreshCountdown[refreshRank]==0)
	{
		commandQueue.needRefresh(refreshRank);
		(*ranks)[refreshRank]->refreshWaiting = true;
		refreshCountdown[refreshRank] =	 REFRESH_PERIOD/tCK;
		refreshRank++;
		if (refreshRank == NUM_RANKS)
		{
			refreshRank = 0;
		}
	}
	//if a rank is powered down, make sure we power it up in time for a refresh
	else if (powerDown[refreshRank] && refreshCountdown[refreshRank] <= tXP)
	{
		(*ranks)[refreshRank]->refreshWaiting = true;
	}

	//pass a pointer to a poppedBusPacket

	//function returns true if there is something valid in poppedBusPacket
	if (commandQueue.pop(&poppedBusPacket))
	{
		if (poppedBusPacket->busPacketType == WRITE || poppedBusPacket->busPacketType == WRITE_P)
		{

			writeDataToSend.push_back(new BusPacket(DATA, poppedBusPacket->physicalAddress, poppedBusPacket->column,
			                                    poppedBusPacket->row, poppedBusPacket->rank, poppedBusPacket->bank,
			                                    poppedBusPacket->data, dramsim_log));
			writeDataCountdown.push_back(WL);
		}

		//
		//update each bank's state based on the command that was just popped out of the command queue
		//
		//for readability's sake
		unsigned rank = poppedBusPacket->rank;
		unsigned bank = poppedBusPacket->bank;
		switch (poppedBusPacket->busPacketType)
		{
			case READ_P:
			case READ:
				//add energy to account for total
				if (DEBUG_POWER)
				{
					PRINT(" ++ Adding Read energy to total energy");
				}
				burstEnergy[rank] += (IDD4R - IDD3N) * BL/2 * NUM_DEVICES;
				if (poppedBusPacket->busPacketType == READ_P) 
				{
					//Don't bother setting next read or write times because the bank is no longer active
					//bankStates[rank][bank].currentBankState = Idle;
					bankStates[rank][bank].nextActivate = max(currentClockCycle + READ_AUTOPRE_DELAY,
							bankStates[rank][bank].nextActivate);
					bankStates[rank][bank].lastCommand = READ_P;
					bankStates[rank][bank].stateChangeCountdown = READ_TO_PRE_DELAY;
				}
				else if (poppedBusPacket->busPacketType == READ)
				{
					bankStates[rank][bank].nextPrecharge = max(currentClockCycle + READ_TO_PRE_DELAY,
							bankStates[rank][bank].nextPrecharge);
					bankStates[rank][bank].lastCommand = READ;

				}

				for (size_t i=0;i<NUM_RANKS;i++)
				{
					for (size_t j=0;j<NUM_BANKS;j++)
					{
						if (i!=poppedBusPacket->rank)
						{
							//check to make sure it is active before trying to set (save's time?)
							if (bankStates[i][j].currentBankState == RowActive)
							{
								bankStates[i][j].nextRead = max(currentClockCycle + BL/2 + tRTRS, bankStates[i][j].nextRead);
								bankStates[i][j].nextWrite = max(currentClockCycle + READ_TO_WRITE_DELAY,
										bankStates[i][j].nextWrite);
							}
						}
						else
						{
							bankStates[i][j].nextRead = max(currentClockCycle + max(tCCD, BL/2), bankStates[i][j].nextRead);
							bankStates[i][j].nextWrite = max(currentClockCycle + READ_TO_WRITE_DELAY,
									bankStates[i][j].nextWrite);
						}
					}
				}

				if (poppedBusPacket->busPacketType == READ_P)
				{
					//set read and write to nextActivate so the state table will prevent a read or write
					//  being issued (in cq.isIssuable())before the bank state has been changed because of the
					//  auto-precharge associated with this command
					bankStates[rank][bank].nextRead = bankStates[rank][bank].nextActivate;
					bankStates[rank][bank].nextWrite = bankStates[rank][bank].nextActivate;
				}

				break;
			case WRITE_P:
			case WRITE:
				if (poppedBusPacket->busPacketType == WRITE_P) 
				{
					bankStates[rank][bank].nextActivate = max(currentClockCycle + WRITE_AUTOPRE_DELAY,
							bankStates[rank][bank].nextActivate);
					bankStates[rank][bank].lastCommand = WRITE_P;
					bankStates[rank][bank].stateChangeCountdown = WRITE_TO_PRE_DELAY;
				}
				else if (poppedBusPacket->busPacketType == WRITE)
				{
					bankStates[rank][bank].nextPrecharge = max(currentClockCycle + WRITE_TO_PRE_DELAY,
							bankStates[rank][bank].nextPrecharge);
					bankStates[rank][bank].lastCommand = WRITE;
				}


				//add energy to account for total
				if (DEBUG_POWER)
				{
					PRINT(" ++ Adding Write energy to total energy");
				}
				burstEnergy[rank] += (IDD4W - IDD3N) * BL/2 * NUM_DEVICES;

				for (size_t i=0;i<NUM_RANKS;i++)
				{
					for (size_t j=0;j<NUM_BANKS;j++)
					{
						if (i!=poppedBusPacket->rank)
						{
							if (bankStates[i][j].currentBankState == RowActive)
							{
								bankStates[i][j].nextWrite = max(currentClockCycle + BL/2 + tRTRS, bankStates[i][j].nextWrite);
								bankStates[i][j].nextRead = max(currentClockCycle + WRITE_TO_READ_DELAY_R,
										bankStates[i][j].nextRead);
							}
						}
						else
						{
							bankStates[i][j].nextWrite = max(currentClockCycle + max(BL/2, tCCD), bankStates[i][j].nextWrite);
							bankStates[i][j].nextRead = max(currentClockCycle + WRITE_TO_READ_DELAY_B,
									bankStates[i][j].nextRead);
						}
					}
				}

				//set read and write to nextActivate so the state table will prevent a read or write
				//  being issued (in cq.isIssuable())before the bank state has been changed because of the
				//  auto-precharge associated with this command
				if (poppedBusPacket->busPacketType == WRITE_P)
				{
					bankStates[rank][bank].nextRead = bankStates[rank][bank].nextActivate;
					bankStates[rank][bank].nextWrite = bankStates[rank][bank].nextActivate;
				}

				break;
			case ACTIVATE:
				//add energy to account for total
				if (DEBUG_POWER)
				{
					PRINT(" ++ Adding Activate and Precharge energy to total energy");
				}
				actpreEnergy[rank] += ((IDD0 * tRC) - ((IDD3N * tRAS) + (IDD2N * (tRC - tRAS)))) * NUM_DEVICES;

				bankStates[rank][bank].currentBankState = RowActive;
				bankStates[rank][bank].lastCommand = ACTIVATE;
				bankStates[rank][bank].openRowAddress = poppedBusPacket->row;
				bankStates[rank][bank].nextActivate = max(currentClockCycle + tRC, bankStates[rank][bank].nextActivate);
				bankStates[rank][bank].nextPrecharge = max(currentClockCycle + tRAS, bankStates[rank][bank].nextPrecharge);

				//if we are using posted-CAS, the next column access can be sooner than normal operation

				bankStates[rank][bank].nextRead = max(currentClockCycle + (tRCD-AL), bankStates[rank][bank].nextRead);
				bankStates[rank][bank].nextWrite = max(currentClockCycle + (tRCD-AL), bankStates[rank][bank].nextWrite);

				for (size_t i=0;i<NUM_BANKS;i++)
				{
					if (i!=poppedBusPacket->bank)
					{
						bankStates[rank][i].nextActivate = max(currentClockCycle + tRRD, bankStates[rank][i].nextActivate);
					}
				}

				break;
			case PRECHARGE:
				bankStates[rank][bank].currentBankState = Precharging;
				bankStates[rank][bank].lastCommand = PRECHARGE;
				bankStates[rank][bank].stateChangeCountdown = tRP;
				bankStates[rank][bank].nextActivate = max(currentClockCycle + tRP, bankStates[rank][bank].nextActivate);

				break;
			case REFRESH:
				//add energy to account for total
				if (DEBUG_POWER)
				{
					PRINT(" ++ Adding Refresh energy to total energy");
				}
				refreshEnergy[rank] += (IDD5 - IDD3N) * tRFC * NUM_DEVICES;

				for (size_t i=0;i<NUM_BANKS;i++)
				{
					bankStates[rank][i].nextActivate = currentClockCycle + tRFC;
					bankStates[rank][i].currentBankState = Refreshing;
					bankStates[rank][i].lastCommand = REFRESH;
					bankStates[rank][i].stateChangeCountdown = tRFC;
				}

				break;
			default:
				ERROR("== Error - Popped a command we shouldn't have of type : " << poppedBusPacket->busPacketType);
				exit(0);
		}

		//issue on bus and print debug
		if (DEBUG_BUS)
		{
			PRINTN(" -- MC Issuing On Command Bus : ");
			poppedBusPacket->print();
		}

		//check for collision on bus
		if (outgoingCmdPacket != NULL)
		{
			ERROR("== Error - Command Bus Collision");
			exit(-1);
		}
		outgoingCmdPacket = poppedBusPacket;
		cmdCyclesLeft = tCMD;

	}

	// SECMC-NI scheduling
 // ********************************
	constructSchedule(currentClockCycle);
	dispatchReq(currentClockCycle);
 // ********************************

	//calculate power
	//  this is done on a per-rank basis, since power characterization is done per device (not per bank)
	for (size_t i=0;i<NUM_RANKS;i++)
	{
		if (USE_LOW_POWER)
		{
			//if there are no commands in the queue and that particular rank is not waiting for a refresh...
			if (commandQueue.isEmpty(i) && !(*ranks)[i]->refreshWaiting)
			{
				//check to make sure all banks are idle
				bool allIdle = true;
				for (size_t j=0;j<NUM_BANKS;j++)
				{
					if (bankStates[i][j].currentBankState != Idle)
					{
						allIdle = false;
						break;
					}
				}

				//if they ARE all idle, put in power down mode and set appropriate fields
				if (allIdle)
				{
					powerDown[i] = true;
					(*ranks)[i]->powerDown();
					for (size_t j=0;j<NUM_BANKS;j++)
					{
						bankStates[i][j].currentBankState = PowerDown;
						bankStates[i][j].nextPowerUp = currentClockCycle + tCKE;
					}
				}
			}
			//if there IS something in the queue or there IS a refresh waiting (and we can power up), do it
			else if (currentClockCycle >= bankStates[i][0].nextPowerUp && powerDown[i]) //use 0 since theyre all the same
			{
				powerDown[i] = false;
				(*ranks)[i]->powerUp();
				for (size_t j=0;j<NUM_BANKS;j++)
				{
					bankStates[i][j].currentBankState = Idle;
					bankStates[i][j].nextActivate = currentClockCycle + tXP;
				}
			}
		}

		//check for open bank
		bool bankOpen = false;
		for (size_t j=0;j<NUM_BANKS;j++)
		{
			if (bankStates[i][j].currentBankState == Refreshing ||
			        bankStates[i][j].currentBankState == RowActive)
			{
				bankOpen = true;
				break;
			}
		}

		//background power is dependent on whether or not a bank is open or not
		if (bankOpen)
		{
			if (DEBUG_POWER)
			{
				PRINT(" ++ Adding IDD3N to total energy [from rank "<< i <<"]");
			}
			backgroundEnergy[i] += IDD3N * NUM_DEVICES;
		}
		else
		{
			//if we're in power-down mode, use the correct current
			if (powerDown[i])
			{
				if (DEBUG_POWER)
				{
					PRINT(" ++ Adding IDD2P to total energy [from rank " << i << "]");
				}
				backgroundEnergy[i] += IDD2P * NUM_DEVICES;
			}
			else
			{
				if (DEBUG_POWER)
				{
					PRINT(" ++ Adding IDD2N to total energy [from rank " << i << "]");
				}
				backgroundEnergy[i] += IDD2N * NUM_DEVICES;
			}
		}
	}

	//check for outstanding data to return to the CPU
	if (returnTransaction.size()>0)
	{
		if (DEBUG_BUS)
		{
			PRINTN(" -- MC Issuing to CPU bus : " << *returnTransaction[0]);
		}
		totalTransactions++;

		bool foundMatch=false;
		//find the pending read transaction to calculate latency
		for (size_t i=0;i<pendingReadTransactions.size();i++)
		{
			if (pendingReadTransactions[i]->address == returnTransaction[0]->address)
			{
				//if(currentClockCycle - pendingReadTransactions[i]->timeAdded > 2000)
				//	{
				//		pendingReadTransactions[i]->print();
				//		exit(0);
				//	}
				unsigned chan,rank,bank,row,col;
				addressMapping(returnTransaction[0]->address,chan,rank,bank,row,col);
				insertHistogram(currentClockCycle-pendingReadTransactions[i]->timeAdded,rank,bank, pendingReadTransactions[i]->core, pendingReadTransactions[i]->isPrefetch);
				//return latency
				returnReadData(pendingReadTransactions[i]);

				delete pendingReadTransactions[i];
				pendingReadTransactions.erase(pendingReadTransactions.begin()+i);
				foundMatch=true; 
				break;
			}
		}
		if (!foundMatch)
		{
			ERROR("Can't find a matching transaction for 0x"<<hex<<returnTransaction[0]->address<<dec);
			abort(); 
		}
		delete returnTransaction[0];
		returnTransaction.erase(returnTransaction.begin());
	}

	//decrement refresh counters
	for (size_t i=0;i<NUM_RANKS;i++)
	{
		refreshCountdown[i]--;
	}

	//
	//print debug
	//
	if (DEBUG_TRANS_Q)
	{
		PRINT("== Printing transaction queue");
		for (size_t i=0;i<transactionQueue.size();i++)
		{
			PRINTN("  " << i << "] "<< *transactionQueue[i]);
		}
	}

	if (DEBUG_BANKSTATE)
	{
		//TODO: move this to BankState.cpp
		PRINT("== Printing bank states (According to MC)");
		for (size_t i=0;i<NUM_RANKS;i++)
		{
			for (size_t j=0;j<NUM_BANKS;j++)
			{
				if (bankStates[i][j].currentBankState == RowActive)
				{
					PRINTN("[" << bankStates[i][j].openRowAddress << "] ");
				}
				else if (bankStates[i][j].currentBankState == Idle)
				{
					PRINTN("[idle] ");
				}
				else if (bankStates[i][j].currentBankState == Precharging)
				{
					PRINTN("[pre] ");
				}
				else if (bankStates[i][j].currentBankState == Refreshing)
				{
					PRINTN("[ref] ");
				}
				else if (bankStates[i][j].currentBankState == PowerDown)
				{
					PRINTN("[lowp] ");
				}
			}
			PRINT(""); // effectively just cout<<endl;
		}
	}

	if (DEBUG_CMD_Q)
	{
		commandQueue.print();
	}

	commandQueue.step();

}

bool MemoryController::WillAcceptTransaction()
{
	return transactionQueue.size() < TRANS_QUEUE_DEPTH;
}

//allows outside source to make request of memory system
bool MemoryController::addTransaction(Transaction *trans)
{
	if (WillAcceptTransaction())
	{
		trans->timeAdded = currentClockCycle;
		transactionQueue.push_back(trans);
		return true;
	}
	else 
	{
		return false;
	}
}

void MemoryController::resetStats()
{

	// for(size_t c=0;c<4;c++){	
	// 	for (size_t i=0; i<NUM_RANKS; i++)
	// 	{
	// 		for (size_t j=0; j<NUM_BANKS; j++)
	// 		{
	// 			//XXX: this means the bank list won't be printed for partial epochs
	// 			grandTotalBankAccesses[SEQUENTIAL(i,j)] += totalReadsPerBank[c][SEQUENTIAL(i,j)] + totalWritesPerBank[c][SEQUENTIAL(i,j)];
	// 			totalReadsPerBank[c][SEQUENTIAL(i,j)] = 0;
	// 			totalWritesPerBank[c][SEQUENTIAL(i,j)] = 0;
	// 			totalEpochLatency[c][SEQUENTIAL(i,j)] = 0;
	// 		}

	// 		burstEnergy[i] = 0;
	// 		actpreEnergy[i] = 0;
	// 		refreshEnergy[i] = 0;
	// 		backgroundEnergy[i] = 0;
	// 		totalReadsPerRank[i] = 0;
	// 		totalWritesPerRank[i] = 0;
	// 	}
	// }

}
//prints statistics at the end of an epoch or  simulation
void MemoryController::printStats(bool finalStats)
{
	unsigned myChannel = parentMemorySystem->systemID;

	unsigned bytesPerTransaction = (JEDEC_DATA_BUS_BITS*BL)/8;
	uint64_t totalBytesTransferred = totalTransactions * bytesPerTransaction;
	double totalSeconds = (double)currentClockCycle * tCK * 1E-9;

	vector <double> bandwidthDemand(NUM_CPU,0.0);
	vector <double> bandwidthPref(NUM_CPU,0.0);

        vector<double> avgCoreLatency(NUM_CPU,0.0);
	vector<double> totalBandwidth(NUM_CPU, 0.0);

        vector<double> avgCoreLatencyPref(NUM_CPU,0.0);
	vector<double> totalBandwidthPref(NUM_CPU, 0.0);

	double totalAggregateBandwidth = 0.0;

	if(finalStats){

		for(size_t c=0;c<NUM_CPU;c++){
			avgCoreLatency[c] = ((double)totalLatency[c] / (double)(totalReads[c])) * tCK;
			avgCoreLatencyPref[c] = ((double)totalLatencyPref[c] / (double)(totalPrefReads[c])) * tCK;
			bandwidthDemand[c] = (((double)(totalReads[c]+(double)totalWrites[c]) * (double)bytesPerTransaction)/(1024.0*1024.0*1024.0)) / totalSeconds;
			bandwidthPref[c] = (((double)(totalPrefReads[c]) * (double)bytesPerTransaction)/(1024.0*1024.0*1024.0)) / totalSeconds;
			totalAggregateBandwidth += bandwidthDemand[c] + bandwidthPref[c];
		}

		cout << " =======================================================" << endl;
		cout << " ============== Printing DRAM Statistics [id:"<<parentMemorySystem->systemID<<"]==============" << endl;
		cout <<  "   Total Return Transactions : " << totalTransactions << endl;
		cout << " ("<<totalBytesTransferred <<" bytes) aggregate average bandwidth "<<totalAggregateBandwidth<<" GB/s" << endl;
		
		for(int core=0;core<NUM_CPU;core++){

			cout << "core " << core << " Demand -- Average bandwidth: "  << bandwidthDemand[core] << " GB/s" << " Average_Latency: " << avgCoreLatency[core] << " ns" << endl;
			cout << "core " << core << " Prefetch -- Average bandwidth: "  << bandwidthPref[core] << " GB/s" << " Average_Latency: " << avgCoreLatencyPref[core] << " ns" << endl;
		}
	}

	
}
MemoryController::~MemoryController()
{
	//ERROR("MEMORY CONTROLLER DESTRUCTOR");
	//abort();
	for (size_t i=0; i<pendingReadTransactions.size(); i++)
	{
		delete pendingReadTransactions[i];
	}
	for (size_t i=0; i<returnTransaction.size(); i++)
	{
		delete returnTransaction[i];
	}

}
//inserts a latency into the latency histogram
void MemoryController::insertHistogram(unsigned latencyValue, unsigned rank, unsigned bank, unsigned core, bool isPrefetch)
{
	// cout << "insertHistogram core" << core << endl;
	if(isPrefetch){
		// totalEpochLatencyPref[core][SEQUENTIAL(rank,bank)] += latencyValue;
		totalLatencyPref[core] += latencyValue;
	}
	else{
		// totalEpochLatency[core][SEQUENTIAL(rank,bank)] += latencyValue;
		totalLatency[core] += latencyValue;
	}
	//poor man's way to bin things.
	latencies[(latencyValue/HISTOGRAM_BIN_SIZE)*HISTOGRAM_BIN_SIZE]++;
}


// to construct SecMC schedule
void MemoryController::constructSchedule(uint64_t curClock)
{
	if(curClock != epochStart)
		return;
	// cout << "turn: " << turn << " epochStart " << epochStart << endl;
	epochStart = curClock + CYCLE_LENGTH;
	
	// copy current schedule to prev schedule
	for(int i=0;i<3;i++)
		for(int j=0;j<4;j++)
			prevSch[i][j] = sch[i][j];

	// reset current schedule
	for(int i=0;i<3;i++)
		for(int j=0;j<4;j++)
			sch[i][j] = max(NUM_RANKS, NUM_BANKS) + 1;

	// change turn
	if(turn == (NUM_CPU - 1))
		turn = 0;
	else
		turn++;	


	// real construction starts here
	// move all requests currently residing on transaction queue to seperate rank queues.
	vector<Transaction *>::iterator	ii;
	for(ii = transactionQueue.begin(); ii != transactionQueue.end();)
	{
		Transaction *transaction = *ii;
		if(transaction->core == turn && !transaction->isPrefetch){ // only add non prefetch requests corresponding to current core
			unsigned newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn;
			addressMapping(transaction->address, newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn);
			// push this transaction in Rank queue and remove from transaction queue
			rankQ[turn][newTransactionRank].push_back(transaction);
			// cout << "adding rank: " << newTransactionRank << " address: " << transaction->address << endl;	
			ii = transactionQueue.erase(ii);
		}
		else
			++ii;
	}	

	// cout << "turn: " << turn << endl;
	// for(int i=0; i < NUM_RANKS;i++){
	// 	cout << "rank: " << i << " size: " << rankQ[turn][i].size() <<  endl;
	// 	cout << "---------------------" << endl;
	// 	for(int j=0; j < rankQ[turn][i].size() ; j++)
	// 	{
	// 		Transaction *transaction = rankQ[turn][i][j];
	// 		// cout << "core: " << transaction->core << endl;
	// 			unsigned newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn;
	// 			addressMapping(transaction->address, newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn);
	// 			cout << "address: " << transaction->address << " rank: " << newTransactionRank << " bank: " << newTransactionBank << endl;
				
	// 	}
	// 	cout << "---------------------" << endl;
	// }	

	// cout << "packets in ranks: ";
	vector <uint64_t > pktsInRank(NUM_RANKS,0);
	for(size_t i=0;i<NUM_RANKS;i++){
		pktsInRank[i] = rankQ[turn][i].size();
		// cout << pktsInRank[i] << " ";
	}
	// cout << endl;

			//cout << "bp2" << endl;	
  	priority_queue<pair<double, int>> q;
	for (int i = 0; i < NUM_RANKS; ++i) {
		q.push(pair<double, int>(pktsInRank[i], i));
	}
	
	// cout << "priority queue: ";
	vector<int> topThree;
	for(int i=0;i<3;i++){
		topThree.push_back(q.top().second);
		// cout << topThree[i] << " ";
		q.pop();
	}
	// cout << endl;
	
			//cout << "bp3" << endl;	
	// Rank re-ordering
	
	for(int i=0;i<3;i++){
		for(int j=0;j<topThree.size();j++){
			if(prevSch[i][0] == topThree[j]){
				sch[i][0] = topThree[j];
				topThree.erase(topThree.begin() + j);
				break;
			}
		}
	}

			//cout << "bp4" << endl;	
	// cout << "remaining size: " << topThree.size() << endl;
	for(int i=0;i<topThree.size();i++){
		for(int j=0;j<3;j++){
			if(sch[j][0] == RB_MAX){
				sch[j][0] = *topThree.begin();
				topThree.erase(topThree.begin());
			}
		}
	}

			//cout << "bp5" << endl;	


	// cout << endl;
	// cout << "rank order: " << prevSch[0][0] << " " << prevSch[1][0] << " " << prevSch[2][0] << endl;
	// cout << "rank order: " << sch[0][0] << " " << sch[1][0] << " " << sch[2][0] << endl;

	// 	cout << "-----------------------------------------" << endl << endl;	
}

void MemoryController::dispatchReq(uint64_t curClock){

	if(curClock != dispatchTick)
		return;
	// cout << "dispatchTick: " << dispatchTick << endl;
	// cout << sch[rankIndx][0] << ", " << sch[rankIndx][bankIndx] << endl;

	// cout << "turn: " << turn << endl;
	// for(int i=0; i < NUM_RANKS;i++){
	// 	cout << "rank: " << i << " size: " << rankQ[turn][i].size() <<  endl;
	// 	cout << "---------------------" << endl;
	// 	for(int j=0; j < rankQ[turn][i].size() ; j++)
	// 	{
	// 		Transaction *transaction = rankQ[turn][i][j];
	// 		// cout << "core: " << transaction->core << endl;
	// 			unsigned newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn;
	// 			addressMapping(transaction->address, newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn);
	// 			cout << "address: " << transaction->address << " rank: " << newTransactionRank << " bank: " << newTransactionBank << endl;
				
	// 	}
	// 	cout << "---------------------" << endl;
	// }
		
        bool emptySlot = true;
	for(int i=0;i<rankQ[turn][sch[rankIndx][0]].size();i++){
		Transaction *transaction = rankQ[turn][sch[rankIndx][0]][i];
		unsigned newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn;
		addressMapping(transaction->address, newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn);

		// bank reordering

		// dispatch if no bank timing violation
		if(noBankViolation(newTransactionBank) && commandQueue.hasRoomFor(2, newTransactionRank, newTransactionBank)){
                        emptySlot = false;
                        assert(!transaction->isPrefetch);
			sch[rankIndx][bankIndx] = newTransactionBank;
			//now that we know there is room in the command queue, we can remove from the transaction queue
			rankQ[turn][sch[rankIndx][0]].erase(rankQ[turn][sch[rankIndx][0]].begin() + i);
		
			//create activate command to the row we just translated
			BusPacket *ACTcommand = new BusPacket(ACTIVATE, transaction->address,
					newTransactionColumn, newTransactionRow, newTransactionRank,
					newTransactionBank, 0, dramsim_log);

			//create read or write command and enqueue it
			BusPacketType bpType = transaction->getBusPacketType();
			BusPacket *command = new BusPacket(bpType, transaction->address,
					newTransactionColumn, newTransactionRow, newTransactionRank,
					newTransactionBank, transaction->data, dramsim_log);


             // update read and writes for stats:
                    if(transaction->transactionType  == DATA_WRITE){
	       		totalWrites[transaction->core]++;
                     }

                    if(transaction->transactionType  == DATA_READ){
                	    totalReads[transaction->core]++;
		    }

			commandQueue.enqueue(ACTcommand);
			commandQueue.enqueue(command);
			// If we have a read, save the transaction so when the data comes back
			// in a bus packet, we can staple it back into a transaction and return it
			if (transaction->transactionType == DATA_READ)
			{
				pendingReadTransactions.push_back(transaction);
			}
			else
			{
				// just delete the transaction now that it's a buspacket
				delete transaction; 
			}
			break;
		}							
		else
		{
				//  go to next iteration
		}
	}

        // add prefetch request if it is an empty slot
        if(emptySlot){
            
            //cout << "empty slot" << endl;
            for (size_t i=0;i<transactionQueue.size();i++)
            {
		Transaction *transaction = transactionQueue[i];
		unsigned newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn;

		addressMapping(transaction->address, newTransactionChan, newTransactionRank, newTransactionBank, newTransactionRow, newTransactionColumn);
        
		if (commandQueue.hasRoomFor(2, newTransactionRank, newTransactionBank) && transaction->isPrefetch)
		{
                    //cout << "prefetch req" << endl;
                    totalPrefReads[transaction->core]++;
                    transactionQueue.erase(transactionQueue.begin()+i);

                    //create activate command to the row we just translated
                    BusPacket *ACTcommand = new BusPacket(ACTIVATE, transaction->address,
                                    newTransactionColumn, newTransactionRow, newTransactionRank,
                                    newTransactionBank, 0, dramsim_log);

                    //create read or write command and enqueue it
                    BusPacketType bpType = transaction->getBusPacketType();

                    BusPacket *command = new BusPacket(bpType, transaction->address,
			newTransactionColumn, newTransactionRow, newTransactionRank,
			newTransactionBank, transaction->data, dramsim_log);



			commandQueue.enqueue(ACTcommand);
			commandQueue.enqueue(command);

			// If we have a read, save the transaction so when the data comes back
			// in a bus packet, we can staple it back into a transaction and return it
			if (transaction->transactionType == DATA_READ)
			{
				pendingReadTransactions.push_back(transaction);
			}
			else
			{
				// just delete the transaction now that it's a buspacket
				delete transaction; 
			}
			/* only allow one transaction to be scheduled per cycle -- this should
			 * be a reasonable assumption considering how much logic would be
			 * required to schedule multiple entries per cycle (parallel data
			 * lines, switching logic, decision logic)
			 */
			break;
                }
            }
        }

	//update rank and bank index in schedule
    if(rankIndx==2){
		rankIndx = 0;
		if(bankIndx==3)
			bankIndx = 1;
	else
		bankIndx++;

	}
	else
		rankIndx++;

	dispatchTick += T_RANK;


	// cout << "current schedule" << endl;
	// for(int i=0;i<3;i++){
	// 		cout << sch[i][0] << " " << sch[i][1] << " " << sch[i][2] << " " << sch[i][3] << " " << endl;
	// 	}

	// cout << "Previous schedule" << endl;
	// for(int i=0;i<3;i++){
	// 		cout << prevSch[i][0] << " " << prevSch[i][1] << " " << prevSch[i][2] << " " << prevSch[i][3] << " " << endl;
	// 	}

}

bool MemoryController::noBankViolation(unsigned bank){
	// same schedule violations
	for(int i=bankIndx-1; i>0; i--){
		if(bank == sch[rankIndx][i])
			return false;
	}

	// prev schedule violations
	if(sch[rankIndx][0] != prevSch[rankIndx][0])
		return true;
	else{
		for(int i=bankIndx+1; i<=3;i++){
			if(bank == prevSch[rankIndx][i])
				return false;
		}
		return true;
	}
}
