## eLua Web Server for microcontrollers (just AVR32 for now)

http://wiki.eluaproject.net/eLuaWebServer

Simply writting and putting some ".html" and ".lua" files inside the SDcard, is possible to manage the micro-controller and our hardware using eLua in order to create dynamic pages.

Using "AJAX" (http://en.wikipedia.org/wiki/Ajax_%28programming%29) techniques the results will be great. 
The project also include an eLua library for manage "JSON" (http://en.wikipedia.org/wiki/Json)  data.

You can get project sources here: https://github.com/nuccioraciti/eLuaWebServer

It was currently tested on a ATEVK100 board using this Linux command line for the building:

nuccio@linux$ scons target=lualong allocator=newlib board=ATEVK1100 optram=0

A little demo (including AJAX test) is present here as file system to put inside the SDCard.

So enjoy with eLuaWebServer, thank you for testing, improving and leaving your feedback for it.

<raciti.nuccio(AT)gmail.com> <Mizar32@http://www.simplemachines.it >

