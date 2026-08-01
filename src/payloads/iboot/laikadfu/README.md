laikaDFU is a single-purpose single-use usb implementation for
receiving pongoOS (or whatever else you are sending it)

We keep it simple:
* Spin up the synopsys PHY
* Take a payload on EP2
* copy it to SRAM base 
* jump to it 

In its current state it's fairly fragile. This isn't my audition for OS development :sweat_smile:, we only have one
host that we care about and it can avoid fucking things up much, and dying in this impl is not the worst. 

