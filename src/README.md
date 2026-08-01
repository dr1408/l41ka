meat:

./exploits  
./payloads/usb_handler - pwneddfu  
./payloads/iboot/patchfinder - iboot patchfinder  
./payloads/iboot/laikadfu - iboot pf jumps to this.  

potatoes:  

./rom-patcher/iBootPatcherSetup.cpp - this talks to the pwneddfu usb handler to set up for a normal nand boot.  
./payloads/PongoOS - we send this to l41ka  

vegetables:

./control - this is the folder where anything that wouldn't be in a generic macOS library goes p much, this does all the
                pi usb client side shit and is also in charge of driving the device. 

