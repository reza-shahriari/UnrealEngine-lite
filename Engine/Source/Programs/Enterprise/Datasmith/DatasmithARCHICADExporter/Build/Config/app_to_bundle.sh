#!/bin/sh

ConfigPath=`dirname "$0"`
pushd $ConfigPath/..
projectPath=`pwd`
cd ../../../../../..
EnginePath=`pwd`
popd

echo "Renaming *.app to *.bundle"
pushd $EnginePath/Binaries/Mac/DatasmithARCHICADExporter
mv DatasmithARCHICAD23Exporter.app DatasmithARCHICAD23Exporter.bundle
mv DatasmithARCHICAD24Exporter.app DatasmithARCHICAD24Exporter.bundle
mv DatasmithARCHICAD25Exporter.app DatasmithARCHICAD25Exporter.bundle
mv DatasmithARCHICAD26Exporter.app DatasmithARCHICAD26Exporter.bundle
mv DatasmithARCHICAD27Exporter.app DatasmithARCHICAD27Exporter.bundle
mv DatasmithARCHICAD28Exporter.app DatasmithARCHICAD28Exporter.bundle
popd
