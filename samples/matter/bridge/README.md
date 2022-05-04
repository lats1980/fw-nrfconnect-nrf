# Matter nRF Connect Bridge Example

This lightening bridge example demonstrates a simple lighting bridge and the use of dynamic endpoints..

The Matter device(nRF5340) that runs the lighting bridge application is connected to the Zigbee device(nRF52840) over serial connection(UART). The Matter device is controlled by the Matter controller device over the Thread protocol. If a Zigbee dimmable lightbulb is commissioned into the Zigbee network, the Matter bridge application will add an endpoint for the bridged lightbulb device. The Matter OnOff cluster command will be interpreted as Zigbee ZCL command and sent to the corresponding bridged device.

## Requirement

* A nRF5340 DK as Matter Bridge.
* A nRF52840 DK as [Zigbee Shell](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/1.9.0/nrf/samples/zigbee/shell/README.html).
* At least one [Zigbee Dimmable Lightbulb](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/1.9.0/nrf/samples/matter/light_bulb/README.html).

## Build the example

This example is based on nRF Conenct SDK **v1.9.0**. Before building the example, install the NCS v1.9.0 first. See [Getting Started](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/1.9.0/nrf/getting_started.html) for more information.

        $ cd ~/nrf/samples/matter
        $ git clone
        $ west build -b nrf5340dk_nrf5340_cpuapp
        $ west flash

## Test the example
