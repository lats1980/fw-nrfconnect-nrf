:orphan:

.. _ncs_release_notes_latest:

Changes in |NCS| v1.4.99
########################

.. contents::
   :local:
   :depth: 2

The most relevant changes that are present on the master branch of the |NCS|, as compared to the latest release, are tracked in this file.



Highlights
**********



Changelog
*********

The following sections provide detailed lists of changes by component.

nRF5
====



nRF9160
=======


  * :ref:`lib_ftp_client` library:

    * Support subset of RFC959 FTP commands only.
    * Added support of STOU and APPE (besides STOR) for "put".
    * Added detection of socket errors, report with proprietary reply message.
    * Increased FTP payload size from NET_IPV4_MTU(576) to MSS as defined on modem side (708).
    * Added polling "226 Transfer complete" after data channel TX/RX, with a configurable timeout of 60 seconds.
    * Ignored the reply code of "UTF8 ON" command as some FTP server returns abnormal reply.

  * :ref:`serial_lte_modem` application:

    * Fixed TCP/UDP port range issue (0~65535).
    * Added AT#XSLEEP=2 to power off UART interface.
    * Added data mode to FTP service.
    * Enabled all SLM services by default.
    * Updated the HTTP client service code to handle chunked HTTP responses.

Common
======




MCUboot
=======






Mcumgr
======





Zephyr
======



Documentation
=============


Samples
-------



User guides
-----------



Known issues
************

Known issues are only tracked for the latest official release.
