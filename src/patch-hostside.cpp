// Copyright Hugh Perkins 2016

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


// This is going to patch the cuda launch instrutions, in the hostside ir. hopefully

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <stdexcept>
#include <sstream>
#include <fstream>

#include "ir-to-opencl-common.h"

using namespace llvm;
using namespace std;

static llvm::LLVMContext TheContext;
static llvm::IRBuilder<> Builder(TheContext);
static std::unique_ptr<llvm::Module> TheModule;

bool single_precision = true;

class LaunchCallInfo {
public:
    LaunchCallInfo() {
        grid_xy_value = 0;
        grid_z_value = 0;
        block_xy_value = 0;
        block_z_value = 0;
    }
    std::string kernelName = "";
    vector<Type *> callTypes;
    vector<Value *> callValuesByValue;
    vector<Value *> callValuesAsPointers;
    Value *grid_xy_value;
    Value *grid_z_value;
    Value *block_xy_value;
    Value *block_z_value;
};

static unique_ptr<LaunchCallInfo> launchCallInfo(new LaunchCallInfo);

ostream &operator<<(ostream &os, const LaunchCallInfo &info) {
    raw_os_ostream my_raw_os_ostream(os);
    my_raw_os_ostream << "LaunchCallInfo " << info.kernelName;
    my_raw_os_ostream << "<<<";
    my_raw_os_ostream << ">>>";
    my_raw_os_ostream << "(";
    int i = 0;
    for(auto it=info.callTypes.begin(); it != info.callTypes.end(); it++) {
        if(i > 0){
            my_raw_os_ostream << ", ";
        }
        Type *type = *it;
        type->print(my_raw_os_ostream);
        i++;
    }
    my_raw_os_ostream << ");\n";
    my_raw_os_ostream << "value types: ";
    i = 0;
    for(auto it=info.callValuesByValue.begin(); it != info.callValuesByValue.end(); it++) {
        Value *value = *it;
        if(i > 0) {
            my_raw_os_ostream << ", ";
        }
        my_raw_os_ostream << dumpType(value->getType());
        i ++;
    }
    // my_raw_os_ostream << "\n";
    return os;
}

void getLaunchTypes(CallInst *inst, LaunchCallInfo *info) {
    // input to this is a cudaLaunch instruction
    // output is:
    // - name of the kernel
    // - type of each of the kernel parameters (without the actual Value's)
    info->callTypes.clear();
    outs() << "getLaunchTypes()\n";
    Value *argOperand = inst->getArgOperand(0);
    if(ConstantExpr *expr = dyn_cast<ConstantExpr>(argOperand)) {
        Instruction *instr = expr->getAsInstruction();
        Type *op0type = instr->getOperand(0)->getType();
        Type *op0typepointed = op0type->getPointerElementType();
        if(FunctionType *fn = dyn_cast<FunctionType>(op0typepointed)) {
            for(auto it=fn->param_begin(); it != fn->param_end(); it++) {
                Type * paramType = *it;
                info->callTypes.push_back(paramType);
            }
        }
        info->kernelName = instr->getOperand(0)->getName();
        outs() << "got kernel name " << info->kernelName << "\n";
    } else {
        throw runtime_error("getlaunchtypes, didnt get ConstantExpr");
    }
}

void getLaunchArgValue(CallInst *inst, LaunchCallInfo *info) {
    // input to this is:
    // - inst is cudaSetupArgument instruction, with:
    //   - first operand is a value pointing to the value we want to send to the kernel
    //
    // - output of this method is
    //    populate info with a Value holding the actual concrete value w ewant to send to the kernel
    //    (note a pointer to it, since we Load the pointer)
    // Notes:
    // - the first operand of inst was created as bitcast(i8*)(alloca (type-of-arg))
    // - the alloca instruction is inst->getOperand(0)->getOperand(0)
    // - so if we load from the alloca instruction, we should have the value we want?
    // outs() << "getLaunchArgValue " << "\n";
    if(!isa<Instruction>(inst->getOperand(0))) {
        outs() << "getlaunchvalue, first operatnd of inst is not an instruction..." << "\n";
        inst->dump();
        outs() << "\n";
        inst->getOperand(0)->dump();
        outs() << "\n";
        throw runtime_error("getlaunchvalue, first operatnd of inst is not an instruction...");
    }
    Instruction *bitcast = cast<Instruction>(inst->getOperand(0));
    Value *alloca = bitcast->getOperand(0);
    Instruction *load = new LoadInst(alloca, "loadCudaArg");
    load->insertBefore(inst);
    info->callValuesByValue.push_back(load);
    info->callValuesAsPointers.push_back(alloca);
}

uint64_t readIntConstant_uint64(ConstantInt *constant) {
    return constant->getZExtValue();
}

uint32_t readIntConstant_uint32(ConstantInt *constant) {
    assert(constant->getBitWidth() <= 32);
    return (uint32_t)constant->getZExtValue();
}

ostream &operator<<(ostream &os, const PointerInfo &pointerInfo) {
    os << "PointerInfo(offset=" << pointerInfo.offset << ", type=" << dumpType(pointerInfo.type);
    os << " indices=";
    int i = 0;
    for(auto it=pointerInfo.indices.begin(); it != pointerInfo.indices.end(); it++) {
        if(i > 0) {
            os << ",";
        }
        os << *it;
        i++;
    }
    os << ")";
    return os;
}

void patchFunction(Function *F) {
    vector<Instruction *> to_replace_with_zero;
    IntegerType *inttype = IntegerType::get(TheContext, 32);
    ConstantInt *constzero = ConstantInt::getSigned(inttype, 0);
    for(auto it=F->begin(); it != F->end(); it++) {
        BasicBlock *basicBlock = &*it;
        for(auto insit=basicBlock->begin(); insit != basicBlock->end(); insit++) {
            if(CallInst *inst = dyn_cast<CallInst>(&*insit)) {
                Function *called = inst->getCalledFunction();
                if(called == 0) {
                    continue;
                }
                if(!called->hasName()) {
                    continue;
                }
                string calledFunctionName = called->getName();
                outs() << "caledFunctionanem " << calledFunctionName << "\n";
                if(calledFunctionName == "cudaLaunch") {
                    outs() << "cudaLaunch\n";
                    getLaunchTypes(inst, launchCallInfo.get());
                    to_replace_with_zero.push_back(inst);
                    outs() << "patching launch in " << string(F->getName()) << "\n";

                    outs() << "cudalaunch kernelanem " << launchCallInfo->kernelName << "\n";
                    Instruction *stringInstr = addStringInstr(F->getParent(), "s." + launchCallInfo->kernelName, launchCallInfo->kernelName);
                    stringInstr->insertBefore(inst);

                    Instruction *clSourcecodeInstr = addStringInstrExistingGlobal(F->getParent(), "__opencl_sourcecode");
                    clSourcecodeInstr->insertBefore(inst);

                    Function *configureKernel = cast<Function>(F->getParent()->getOrInsertFunction(
                        "configureKernel",
                        Type::getVoidTy(TheContext),
                        PointerType::get(IntegerType::get(TheContext, 8), 0),
                        PointerType::get(IntegerType::get(TheContext, 8), 0),
                        NULL));
                    Value *args[2];
                    args[0] = stringInstr;
                    args[1] = clSourcecodeInstr;
                    CallInst *callLaunch = CallInst::Create(configureKernel, ArrayRef<Value *>(&args[0], &args[2]));
                    callLaunch->insertBefore(inst);

                    Instruction *lastInst = callLaunch;
                    // pass args now
                    int i = 0;
                    for(auto argit=launchCallInfo->callValuesByValue.begin(); argit != launchCallInfo->callValuesByValue.end(); argit++) {
                        Value *value = *argit;
                        Value *valueAsPointerInstr = launchCallInfo->callValuesAsPointers[i];
                        if(IntegerType *intType = dyn_cast<IntegerType>(value->getType())) {
                            int bitLength = intType->getBitWidth();
                            outs() << "bitLength " << bitLength << "\n";
                            string mangledName = "";
                            if(bitLength == 32) {
                                // typeabbrev = "i";
                                mangledName = "_Z17setKernelArgInt32i";
                            } else if(bitLength == 64) {
                                // typeabbrev = "l";
                                mangledName = "_Z17setKernelArgInt64l";
                            } else {
                                throw runtime_error("bitlength " + toString(bitLength) + " not implemented");
                            }
                            Function *setKernelArgInt = cast<Function>(F->getParent()->getOrInsertFunction(
                                mangledName,
                                Type::getVoidTy(TheContext),
                                IntegerType::get(TheContext, bitLength),
                                NULL));
                            CallInst *call = CallInst::Create(setKernelArgInt, value);
                            call->insertAfter(lastInst);
                            lastInst = call;
                        } else if(value->getType()->isFloatingPointTy()) {
                            Function *setKernelArgFloat = cast<Function>(F->getParent()->getOrInsertFunction(
                                "_Z17setKernelArgFloatf",
                                Type::getVoidTy(TheContext),
                                Type::getFloatTy(TheContext),
                                NULL));
                            CallInst *call = CallInst::Create(setKernelArgFloat, value);
                            call->insertAfter(lastInst);
                            lastInst = call;
                        } else if(value->getType()->isPointerTy()) {
                            Type *elementType = value->getType()->getPointerElementType();
                            if(elementType->isFloatingPointTy()) {
                                Function *setKernelArgFloatStar = cast<Function>(F->getParent()->getOrInsertFunction(
                                    "_Z21setKernelArgFloatStarPf",
                                    Type::getVoidTy(TheContext),
                                    PointerType::get(Type::getFloatTy(TheContext), 0),
                                    NULL));
                                CallInst *call = CallInst::Create(setKernelArgFloatStar, value);
                                call->insertAfter(lastInst);
                                lastInst = call;
                            }
                        } else if(isa<StructType>(value->getType())) {
                            outs() << "got a struct" << "\n";

                            // lets just statically analyse the struct for now, without thinking how we're going to
                            // actually deal with it
                            // we want to know things like:
                            // - how big it is?
                            // - does it contain any pointers?  to what?  cpu memory? gpu memory? floats? structs?

                            // what we're going to do with this information:
                            // - at runtime, allocate a cl_mem suffiicnelty large to hold any cpu memory we want to send
                            //   (assuming we're going to send this struct by-value basically)
                            // - copy the struct to this buffer
                            // - send this buffer into the kernel, as an argument
                            //
                            // after running the kernel, presuambly we'll need to clean up this memory
                            // actually easycl sort of handles some of this stuff already..
                            // anyway, the hard bit will be:
                            // - getting hold of infomration about the struct
                            // - dealing with pointers insdie the struct

                            // lets start by getting the size of the struct
                            // from https://stackoverflow.com/questions/14608250/how-can-i-find-the-size-of-a-type/30830445#30830445 ,
                            // we can get this size like this:
                            // %Size = getelementptr %T* null, i32 1
                            // %SizeI = ptrtoint %T* %Size to i32
                            // ... so let's do that

                            // ... hmmmm.... thats at runtime, but we are not at runtime now, so ...
                            // https://stackoverflow.com/questions/14608250/how-can-i-find-the-size-of-a-type/14608251#14608251
                            // (same thread as the other) seems to apply whilst statically analyzing llvm code
                            // getTypeAllocSize getTypeAllocSizeInBits
                            // In LLVM versions 3.2 and above, the DataLayout type replaces TargetData

                            Module *M = F->getParent();
                            const DataLayout *dataLayout = &M->getDataLayout();
                            int allocSize = dataLayout->getTypeAllocSize(value->getType());
                            outs() << "typeallocsize " << allocSize << "\n";
                            // we could just naively allocate this, and copy it to the kernel, but superficial inspection
                            // of the target for eigen shows it contains a float *.  that probably points into gpu
                            // memory already.  We'd probably better scan for that.
                            unique_ptr<StructInfo> structInfo(new StructInfo());
                            walkStructType(TheModule.get(), structInfo.get(), 0, 0, vector<int>(), "", cast<StructType>(value->getType()));

                            // now we need to set up instructions to pass in:
                            // - the struct itself
                            // - each of the float arrays
                            // 
                            // For the struct itself, at runtime, we're going to have to:
                            // - allocate a cl_mem for it
                            // - copy the struct cpu memory to the cl_mem
                            // - pass the cl_mem into the kernel, as an argument
                            //
                            // So, for the struct, at patch time, we probalby need to call a function like:
                            // - setKernelArgStruct(char *pCpuStruct, int structAllocateSize);

                            Function *setKernelArgStruct = cast<Function>(F->getParent()->getOrInsertFunction(
                                "_Z18setKernelArgStructPci",
                                Type::getVoidTy(TheContext),
                                PointerType::get(IntegerType::get(TheContext, 8), 0),
                                IntegerType::get(TheContext, 32),
                                NULL));

                            BitCastInst *bitcast = new BitCastInst(valueAsPointerInstr, PointerType::get(IntegerType::get(TheContext, 8), 0));
                            bitcast->insertAfter(lastInst);
                            lastInst = bitcast;

                            Value *args[2];
                            args[0] = bitcast;
                            args[1] = createInt32Constant(&TheContext, allocSize);

                            CallInst *call = CallInst::Create(setKernelArgStruct, ArrayRef<Value *>(args));
                            call->insertAfter(lastInst);
                            lastInst = call;

                            // pass in all the float stars:
                            outs() << "pointers in struct:" << "\n";
                            for(auto pointerit=structInfo->pointerInfos.begin(); pointerit != structInfo->pointerInfos.end(); pointerit++) {
                                PointerInfo *pointerInfo = pointerit->get();
                                int offset = pointerInfo->offset;
                                Type *type = pointerInfo->type;
                                vector<Value *> indices;
                                // add a leading 0:
                                indices.push_back(createInt32Constant(&TheContext, 0));
                                for(auto idxit = pointerInfo->indices.begin(); idxit != pointerInfo->indices.end(); idxit++) {
                                    int idx = *idxit;
                                    outs() << "idx " << idx << "\n";
                                    indices.push_back(createInt32Constant(&TheContext, idx));
                                }
                                GetElementPtrInst *gep = GetElementPtrInst::CreateInBounds(value->getType(), valueAsPointerInstr, ArrayRef<Value *>(&indices[0], &indices[indices.size()]), "getfloatstaraddr");
                                outs() << "gep type " << dumpType(gep->getType()) << "\n";
                                gep->insertAfter(lastInst);
                                lastInst = gep;

                                // I guess we need to load the value from that gep address now?
                                LoadInst *loadgep = new LoadInst(gep, "loadgep");
                                loadgep->insertAfter(lastInst);
                                lastInst = loadgep;

                                outs() << "loadgep type " << dumpType(loadgep->getType()) << "\n";

                                // we're just going to assume everything is a float* for now
                                Function *setKernelArgFloatStar = cast<Function>(F->getParent()->getOrInsertFunction(
                                    "_Z21setKernelArgFloatStarPf",
                                    Type::getVoidTy(TheContext),
                                    PointerType::get(Type::getFloatTy(TheContext), 0),
                                    NULL));
                                CallInst *call = CallInst::Create(setKernelArgFloatStar, loadgep);
                                call->insertAfter(lastInst);
                                lastInst = call;
                            }
                        } else {
                            throw runtime_error("type not implemented " + dumpType(value->getType()));
                        }
                        i++;
                    }
                    // trigger the kernel...
                    Function *kernelGo = cast<Function>(F->getParent()->getOrInsertFunction(
                        "_Z8kernelGov",
                        Type::getVoidTy(TheContext),
                        NULL));
                    CallInst *kernelGoInst = CallInst::Create(kernelGo);
                    kernelGoInst->insertAfter(lastInst);
                    lastInst = kernelGoInst;

                    launchCallInfo->callValuesByValue.clear();
                    launchCallInfo->callValuesAsPointers.clear();
                } else if(calledFunctionName == "cudaSetupArgument") {
                    outs() << "cudaSetupArgument\n";
                    getLaunchArgValue(inst, launchCallInfo.get());
                    to_replace_with_zero.push_back(inst);
                }
            }
        }
    }
    for(auto it=to_replace_with_zero.begin(); it != to_replace_with_zero.end(); it++) {
        Instruction *inst = *it;
        BasicBlock::iterator ii(inst);
        ReplaceInstWithValue(inst->getParent()->getInstList(), ii, constzero);
    }
}


void patchModule(string deviceclfilename, Module *M) {
    // add in opencl sourcecode
    ifstream f_in(deviceclfilename);
    string cl_sourcecode(
        (std::istreambuf_iterator<char>(f_in)),
        (std::istreambuf_iterator<char>()));

    addGlobalVariable(M, "__opencl_sourcecode", cl_sourcecode);

    vector<Function *> functionsToRemove;
    for(auto it = M->begin(); it != M->end(); it++) {
        Function *F = &*it;
        string name = F->getName();
    }
}

int main(int argc, char *argv[]) {
    SMDiagnostic Err;
    if(argc != 4) {
        outs() << "Usage: " << argv[0] << " infile-rawhost.ll infile-device.cl outfile-patchedhost.ll" << "\n";
        return 1;
    }

    string rawhostfilename = argv[1];
    string deviceclfilename = argv[2];
    string patchedhostfilename = argv[3];
    outs() << "reading rawhost ll file " << rawhostfilename << "\n";
    outs() << "reading device cl file " << deviceclfilename << "\n";
    outs() << "outputing to patchedhost file " << patchedhostfilename << "\n";

    TheModule = parseIRFile(rawhostfilename, Err, TheContext);
    if(!TheModule) {
        Err.print(argv[0], errs());
        return 1;
    }

    patchModule(deviceclfilename, TheModule.get());

    AssemblyAnnotationWriter assemblyAnnotationWriter;
    ofstream ofile;
    ofile.open(patchedhostfilename);
    raw_os_ostream my_raw_os_ostream(ofile);
    verifyModule(*TheModule);
    TheModule->print(my_raw_os_ostream, &assemblyAnnotationWriter);
    ofile.close();
    return 0;
}
