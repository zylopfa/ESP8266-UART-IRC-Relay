# ESP8266, IRC to Serial/UART Bridge

This program is used to bridge a microcontroller to the wifi module ESP8266 via
serial link / UART.

The ESP8266, when the microncontroller enable it, will through this program,
connect to the specified accesspoint (hardcoded now). When connected to the AP,
 the ESP8266 will connect to the specified irc server (ip,port,nick,channel) and
will wait for commands.


## IRC Commands

Commands are privmsg'ed to the bot, like in most IRC clients:

```
 /msg BotName !TESTUART
```

Below you can see which commands + parameters the irc bot will take.

#### !TESTUART

```
 /msg BotName !TESTUART
```

This will make the irc bot send the string "!TESTUART" through the ESP8266's UART tx line
to the microcontroller connected to it.
This is used for testing purposes in your microcontroller (MCU) and it up to the MCU to
do whatever, in order to show you, the UART link is working.


#### !UART

```
 /msg BotName !UART <command>
```

This will send the <command> text directly to the MCU via UART.
This way we can let the MCU do whatever actions it need to based on the incomming <command>
text. Baically we use the ESP8266 as an IRC relay of commands, so it is the MCU's job
to act on, or discard commands.

This way, we can program the ESP 1 time only and let the MCU deal with general programming
logic.

On the MCU you have to setup UART and setup interrupt handler to handle incomming commands.


## Notes

This program contain a few extra irc commands a its based on my earlier work, it will
be cleaned up in the future, to only contain the UART functionality.

