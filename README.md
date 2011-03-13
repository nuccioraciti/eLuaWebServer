## eLua Web Server for microcontrollers (just AVR32 for now)

The ultimate goal of this project is to be able to manage through a Web-browser, a cheap hardware in a very simple way, without expensive hardware for programming or proprietary softwares.
This is the reason I chose the "Mizar32" board, it is economic and thanks to "eLuaWebServer"  it can be programmed (I hope) by simply putting files inside his SDCard.

It currently has been tested on a ATEVK100 board using  this command line for the building:
scons target=lualong allocator=newlib board=ATEVK1100 optram=0

So enjoy with eLuaWebServer, and many thanks to the eLua development team.

<raciti.nuccio(AT)gmail.com> <Mizar32@http://www.simplemachines.it >

