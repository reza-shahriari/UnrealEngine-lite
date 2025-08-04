<?xml version="1.0" encoding="utf-8"?>
<TpsData xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <Name>Open MPI</Name>
  <!-- Software Name and Version  -->
<!-- Software Name: Open MPI
    Download Link: https://github.com/open-mpi/ompi
    Version:  
    Notes: The onnxruntime library consists of header files and
  dynamically linked libraries of which this software is part of. Both headers
  and shared libraries will be added and used in the engine. While the headers
  are compiled into the standalone application, the dynamic libraries are
  distributed along with it. The software is thus used internally, by licensees
  as well as end consumers.
        -->
<Location>/Engine/Plugins/NNE/NNERuntimeORT/Source/ThirdParty/Onnxruntime/</Location>
<Function>The software is part of the Onnxruntime library, which is
  used to run neural network inference through the onnx runtime backend and
  also to optimize ML models.</Function>
<Eula>https://github.com/open-mpi/ompi/blob/main/LICENSE</Eula>
  <RedistributeTo>
    <EndUserGroup>Licensee</EndUserGroup>
    <EndUserGroup>P4</EndUserGroup>
    <EndUserGroup>Git</EndUserGroup>
  </RedistributeTo>
  <LicenseFolder>/Engine/Source/ThirdParty/Licenses</LicenseFolder>
</TpsData>