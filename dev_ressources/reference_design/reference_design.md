# Reference design

https://www.instructables.com/DIY-REFLOW-OVEN/

This firmware ports the STM32 + Nextion reflow controller from that Instructable
feature-for-feature. Its sources (the STM32CubeIDE project
`Reflow_Oven_STM32_V1_1/` and the Nextion HMI file `Reflow.HMI`) are not
redistributed in this repository, because their license could not be
confirmed. Download them from the page above. The design spec links into
`Reflow_Oven_STM32_V1_1/Core/Src/main.c`; those links resolve once the project
is unpacked into this directory, where `.gitignore` keeps it out of commits.
