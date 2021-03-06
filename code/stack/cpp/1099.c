// ==++==
// 
//   
//    Copyright (c) 2006 Microsoft Corporation.  All rights reserved.
//   
//    The use and distribution terms for this software are contained in the file
//    named license.txt, which can be found in the root of this distribution.
//    By using this software in any fashion, you are agreeing to be bound by the
//    terms of this license.
//   
//    You must not remove this notice, or any other, from this software.
//   
// 
// ==--==
//*****************************************************************************
// File: stack.cpp
//
// CLRData stack walking.
//
//*****************************************************************************

#include "stdafx.h"

//----------------------------------------------------------------------------
//
// ClrDataStackWalk.
//
//----------------------------------------------------------------------------

ClrDataStackWalk::ClrDataStackWalk(ClrDataAccess* dac,
                                   Thread* thread,
                                   ULONG32 flags)
{
    m_dac = dac;
    m_dac->AddRef();
    m_instanceAge = m_dac->m_instanceAge;
    m_thread = thread;
    m_walkFlags = flags;
    m_refs = 1;
    m_stackPrev = 0;
}

ClrDataStackWalk::~ClrDataStackWalk(void)
{
    m_dac->Release();
}

STDMETHODIMP
ClrDataStackWalk::QueryInterface(THIS_
                                 IN REFIID interfaceId,
                                 OUT PVOID* iface)
{
    if (IsEqualIID(interfaceId, IID_IUnknown) ||
        IsEqualIID(interfaceId, __uuidof(IXCLRDataStackWalk)))
    {
        AddRef();
        *iface = static_cast<IUnknown*>
            (static_cast<IXCLRDataStackWalk*>(this));
        return S_OK;
    }
    else
    {
        *iface = NULL;
        return E_NOINTERFACE;
    }
}

STDMETHODIMP_(ULONG)
ClrDataStackWalk::AddRef(THIS)
{
    return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG)
ClrDataStackWalk::Release(THIS)
{
    LONG newRefs = InterlockedDecrement(&m_refs);
    if (newRefs == 0)
    {
        delete this;
    }
    return newRefs;
}

HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::GetContext( 
    /* [in] */ ULONG32 contextFlags,
    /* [in] */ ULONG32 contextBufSize,
    /* [out] */ ULONG32 *contextSize,
    /* [size_is][out] */ BYTE contextBuf[  ])
{
    HRESULT status;

    if (contextSize)
    {
        *contextSize = ContextSizeForFlags(contextFlags);
    }
    
    if (!CheckContextSizeForFlags(contextBufSize, contextFlags))
    {
        return E_INVALIDARG;
    }
    
    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (!m_frameIter.IsValid())
        {
            status = S_FALSE;
        }
        else
        {
            *(PCONTEXT)contextBuf = m_context;
            UpdateContextFromRegDisp(&m_regDisp, (PCONTEXT)contextBuf);
            status = S_OK;
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}

HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::SetContext( 
    /* [in] */ ULONG32 contextSize,
    /* [size_is][in] */ BYTE context[  ])
{
    return SetContext2(m_frameIter.m_crawl.IsActiveFrame() ?
                       CLRDATA_STACK_SET_CURRENT_CONTEXT :
                       CLRDATA_STACK_SET_UNWIND_CONTEXT,
                       contextSize, context);
}

HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::SetContext2( 
    /* [in] */ ULONG32 flags,
    /* [in] */ ULONG32 contextSize,
    /* [size_is][in] */ BYTE context[  ])
{
    HRESULT status;

    if ((flags & ~(CLRDATA_STACK_SET_CURRENT_CONTEXT |
                   CLRDATA_STACK_SET_UNWIND_CONTEXT)) != 0 ||
        !CheckContextSizeForInBuffer(contextSize, context))
    {
        return E_INVALIDARG;
    }
    
    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        // Copy the context to local state so
        // that its lifetime extends beyond this call.
        m_context = *(PCONTEXT)context;
        m_thread->FillRegDisplay(&m_regDisp, &m_context);
        m_frameIter.
            SetIsFirstFrame((flags & CLRDATA_STACK_SET_CURRENT_CONTEXT) != 0);
        m_frameIter.ResetRegDisp(&m_regDisp);
        m_stackPrev = (TADDR)GetRegdisplaySP(&m_regDisp);
        FilterFrames();
        status = S_OK;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}

HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::Next(void)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (!m_frameIter.IsValid())
        {
            status = S_FALSE;
        }
        else
        {
            // Default the previous stack value.
            m_stackPrev = (TADDR)GetRegdisplaySP(&m_regDisp);
            StackWalkAction action = m_frameIter.Next();
            switch(action)
            {
            case SWA_CONTINUE:
                // We sucessfully unwound a frame so update
                // the previous stack pointer before going into
                // filtering to get the amount of stack skipped
                // by the filtering.
                m_stackPrev = (TADDR)GetRegdisplaySP(&m_regDisp);
                FilterFrames();
                status = m_frameIter.IsValid() ? S_OK : S_FALSE;
                break;
            case SWA_ABORT:
                status = S_FALSE;
                break;
            default:
                status = E_FAIL;
                break;
            }
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::GetStackSizeSkipped( 
    /* [out] */ ULONG64 *stackSizeSkipped)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (m_stackPrev)
        {
            *stackSizeSkipped =
                (TADDR)GetRegdisplaySP(&m_regDisp) - m_stackPrev;
            status = S_OK;
        }
        else
        {
            status = S_FALSE;
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}

HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::GetFrameType( 
    /* [out] */ CLRDataSimpleFrameType *simpleType,
    /* [out] */ CLRDataDetailedFrameType *detailedType)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (m_frameIter.IsValid())
        {
            RawGetFrameType(simpleType, detailedType);
            status = S_OK;
        }
        else
        {
            status = S_FALSE;
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}

HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::GetFrame( 
    /* [out] */ IXCLRDataFrame **frame)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        ClrDataFrame* dataFrame = NULL;
        if (!m_frameIter.IsValid())
        {
            status = E_INVALIDARG;
            goto Exit;
        }
        
        CLRDataSimpleFrameType simpleType;
        CLRDataDetailedFrameType detailedType;

        RawGetFrameType(&simpleType, &detailedType);
        dataFrame =
            new (nothrow) ClrDataFrame(m_dac, simpleType, detailedType,
                                       m_frameIter.m_crawl.GetAppDomain(),
                                       m_frameIter.m_crawl.GetFunction());
        if (!dataFrame)
        {
            status = E_OUTOFMEMORY;
            goto Exit;
        }
        
        dataFrame->m_context = m_context;
        UpdateContextFromRegDisp(&m_regDisp, &dataFrame->m_context);
        m_thread->FillRegDisplay(&dataFrame->m_regDisp,
                                 &dataFrame->m_context);

        *frame = static_cast<IXCLRDataFrame*>(dataFrame);
        status = S_OK;
        
    Exit: ;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataStackWalk::Request( 
    /* [in] */ ULONG32 reqCode,
    /* [in] */ ULONG32 inBufferSize,
    /* [size_is][in] */ BYTE *inBuffer,
    /* [in] */ ULONG32 outBufferSize,
    /* [size_is][out] */ BYTE *outBuffer)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        switch(reqCode)
        {
        case CLRDATA_REQUEST_REVISION:
            if (inBufferSize != 0 ||
                inBuffer ||
                outBufferSize != sizeof(ULONG32))
            {
                status = E_INVALIDARG;
            }
            else
            {
                *(ULONG32*)outBuffer = 1;
                status = S_OK;
            }
            break;

        case CLRDATA_STACK_WALK_REQUEST_SET_FIRST_FRAME:
            if ((inBufferSize != sizeof(ULONG32)) ||
                (outBufferSize != 0))
            {
                status = E_INVALIDARG;
                break;
            }

            m_frameIter.SetIsFirstFrame(*(ULONG32 UNALIGNED *)inBuffer != 0);
            status = S_OK;
            break;
            
        case DACSTACKPRIV_REQUEST_FRAME_DATA:
            if ((inBufferSize != 0) ||
                (inBuffer != NULL) ||
                (outBufferSize != sizeof(DacpFrameData)))
            {
                status = E_INVALIDARG;
                break;
            }
            if (!m_frameIter.IsValid())
            {
                status = E_INVALIDARG;
                break;
            }
            
            DacpFrameData* frameData;

            frameData = (DacpFrameData*)outBuffer;
            frameData->frameAddr =
                TO_CDADDR(PTR_HOST_TO_TADDR(m_frameIter.m_crawl.GetFrame()));
            status = S_OK;
            break;
            
        default:
            status = E_INVALIDARG;
            break;
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}

HRESULT
ClrDataStackWalk::Init(void)
{
    if (m_thread->IsUnstarted())
    {
        return E_FAIL;
    }

    if (m_thread->GetFilterContext())
    {
        m_context = *m_thread->GetFilterContext();
    }
    else
    {
        DacGetThreadContext(m_thread, &m_context);
    }
    m_thread->FillRegDisplay(&m_regDisp, &m_context);

    m_stackPrev = (TADDR)GetRegdisplaySP(&m_regDisp);

    ULONG32 iterFlags = 0;

    // If the filter is only allowing method frames
    // turn on the appropriate iterator flag.
    if ((m_walkFlags & SIMPFRAME_ALL) ==
        CLRDATA_SIMPFRAME_MANAGED_METHOD)
    {
        iterFlags |= FUNCTIONSONLY;
    }
    
    m_frameIter.Init(m_thread, NULL, &m_regDisp, iterFlags);
    if (m_frameIter.m_frameState == StackFrameIterator::SFITER_UNINITIALIZED)
    {
        return E_FAIL;
    }
    FilterFrames();

    return S_OK;
}

void
ClrDataStackWalk::FilterFrames(void)
{
    //
    // Advance to a state compatible with the
    // current filtering flags.
    //

    while (m_frameIter.IsValid())
    {
        switch(m_frameIter.m_frameState)
        {
        case StackFrameIterator::SFITER_FRAMELESS_METHOD:
            if (m_walkFlags & CLRDATA_SIMPFRAME_MANAGED_METHOD)
            {
                return;
            }
            break;
        case StackFrameIterator::SFITER_FRAME_FUNCTION:
        case StackFrameIterator::SFITER_SKIPPED_FRAME_FUNCTION:
            if (m_walkFlags & CLRDATA_SIMPFRAME_RUNTIME_UNMANAGED_CODE)
            {
                return;
            }
            break;
        default:
            break;
        }

        m_frameIter.Next();
    }
}

void
ClrDataStackWalk::UpdateContextFromRegDisp(PREGDISPLAY regDisp,
                                           PCONTEXT context)
{
#if defined(_X86_)
    context->Edi = *regDisp->pEdi;
    context->Esi = *regDisp->pEsi;
    context->Ebx = *regDisp->pEbx;
    context->Ebp = *regDisp->pEbp;
    context->Eax = *regDisp->pEax;
    context->Ecx = *regDisp->pEcx;
    context->Edx = *regDisp->pEdx;
    context->Esp = regDisp->Esp;
    context->Eip = (ULONG)(ULONG_PTR)*regDisp->pPC;
#else
    EX_THROW(HRException, (E_NOTIMPL));
#endif
}

void
ClrDataStackWalk::RawGetFrameType(
    /* [out] */ CLRDataSimpleFrameType* simpleType,
    /* [out] */ CLRDataDetailedFrameType* detailedType)
{
    if (simpleType)
    {
        switch(m_frameIter.m_frameState)
        {
        case StackFrameIterator::SFITER_FRAMELESS_METHOD:
            *simpleType = CLRDATA_SIMPFRAME_MANAGED_METHOD;
            break;
        case StackFrameIterator::SFITER_FRAME_FUNCTION:
        case StackFrameIterator::SFITER_SKIPPED_FRAME_FUNCTION:
            *simpleType = CLRDATA_SIMPFRAME_RUNTIME_UNMANAGED_CODE;
            break;
        default:
            *simpleType = CLRDATA_SIMPFRAME_UNRECOGNIZED;
            break;
        }
    }

    if (detailedType)
    {
        // XXX drewb - Detailed type.
        *detailedType = CLRDATA_DETFRAME_UNRECOGNIZED;
    }
}

//----------------------------------------------------------------------------
//
// ClrDataFrame.
//
//----------------------------------------------------------------------------

ClrDataFrame::ClrDataFrame(ClrDataAccess* dac,
                           CLRDataSimpleFrameType simpleType,
                           CLRDataDetailedFrameType detailedType,
                           AppDomain* appDomain,
                           MethodDesc* methodDesc)
{
    m_dac = dac;
    m_dac->AddRef();
    m_instanceAge = m_dac->m_instanceAge;
    m_simpleType = simpleType;
    m_detailedType = detailedType;
    m_appDomain = appDomain;
    m_methodDesc = methodDesc;
    m_refs = 1;
    m_methodSig = NULL;
    m_localSig = NULL;
}

ClrDataFrame::~ClrDataFrame(void)
{
    delete m_methodSig;
    delete m_localSig;
    m_dac->Release();
}

STDMETHODIMP
ClrDataFrame::QueryInterface(THIS_
                             IN REFIID interfaceId,
                             OUT PVOID* iface)
{
    if (IsEqualIID(interfaceId, IID_IUnknown) ||
        IsEqualIID(interfaceId, __uuidof(IXCLRDataFrame)))
    {
        AddRef();
        *iface = static_cast<IUnknown*>
            (static_cast<IXCLRDataFrame*>(this));
        return S_OK;
    }
    else
    {
        *iface = NULL;
        return E_NOINTERFACE;
    }
}

STDMETHODIMP_(ULONG)
ClrDataFrame::AddRef(THIS)
{
    return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG)
ClrDataFrame::Release(THIS)
{
    LONG newRefs = InterlockedDecrement(&m_refs);
    if (newRefs == 0)
    {
        delete this;
    }
    return newRefs;
}

HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetContext( 
    /* [in] */ ULONG32 contextFlags,
    /* [in] */ ULONG32 contextBufSize,
    /* [out] */ ULONG32 *contextSize,
    /* [size_is][out] */ BYTE contextBuf[  ])
{
    HRESULT status;

    if (contextSize)
    {
        *contextSize = ContextSizeForFlags(contextFlags);
    }
    
    if (!CheckContextSizeForFlags(contextBufSize, contextFlags))
    {
        return E_INVALIDARG;
    }
    
    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        *(PCONTEXT)contextBuf = m_context;
        status = S_OK;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetFrameType( 
    /* [out] */ CLRDataSimpleFrameType *simpleType,
    /* [out] */ CLRDataDetailedFrameType *detailedType)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        *simpleType = m_simpleType;
        *detailedType = m_detailedType;
        status = S_OK;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetAppDomain( 
    /* [out] */ IXCLRDataAppDomain **appDomain)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (m_appDomain)
        {
            ClrDataAppDomain* dataAppDomain =
                new (nothrow) ClrDataAppDomain(m_dac, m_appDomain);
            if (!dataAppDomain)
            {
                status = E_OUTOFMEMORY;
            }
            else
            {
                *appDomain = static_cast<IXCLRDataAppDomain*>(dataAppDomain);
                status = S_OK;
            }
        }
        else
        {
            *appDomain = NULL;
            status = S_FALSE;
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetNumArguments( 
    /* [out] */ ULONG32 *numArgs)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (!m_methodDesc)
        {
            status = E_NOINTERFACE;
        }
        else
        {
            MetaSig* sig;

            status = GetMethodSig(&sig, numArgs);
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetArgumentByIndex( 
    /* [in] */ ULONG32 index,
    /* [out] */ IXCLRDataValue **arg,
    /* [in] */ ULONG32 bufLen,
    /* [out] */ ULONG32 *nameLen,
    /* [size_is][out] */ WCHAR name[  ])
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (nameLen)
        {
            *nameLen = 0;
        }
        
        if (!m_methodDesc)
        {
            status = E_NOINTERFACE;
            goto Exit;
        }

        MetaSig* sig;
        ULONG32 numArgs;

        if (FAILED(status = GetMethodSig(&sig, &numArgs)))
        {
            goto Exit;
        }

        if (index >= numArgs)
        {
            status = E_INVALIDARG;
            goto Exit;
        }

        if ((bufLen && name) || nameLen)
        {
            if (index == 0 && sig->HasThis())
            {
                if (nameLen)
                {
                    *nameLen = 5;
                }


                StringCchCopy(name, bufLen, L"this");
                
            }
            else
            {
                if (!m_methodDesc->IsNoMetadata())
                {
                    IMDInternalImport* mdImport = m_methodDesc->GetMDImport();
                    mdParamDef paramToken;
                    LPCSTR paramName;
                    USHORT seq;
                    DWORD attr;

                    // Param indexing is 1-based.
                    ULONG32 mdIndex = index + 1;

                    // 'this' doesn't show up in the signature but
                    // is present in the dac API indexing so adjust the
                    // index down for methods with 'this'.
                    if (sig->HasThis())
                    {
                        mdIndex--;
                    }
                    
                    if ((status = mdImport->
                         FindParamOfMethod(m_methodDesc->GetMemberDef(),
                                           mdIndex,
                                           &paramToken)) == S_OK &&
                        (paramName = mdImport->
                         GetParamDefProps(paramToken, &seq, &attr)))
                    {
                        if ((status = ConvertUtf8(paramName,
                                                  bufLen, nameLen, name)) != S_OK)
                        {
                            goto Exit;
                        }
                    }
                }
                else
                {
                    if (nameLen)
                    {
                        *nameLen = 1;
                    }

                    name[0] = 0;
                }
            }
        }
        
        status = ValueFromDebugInfo(sig, true, index, index, arg);

    Exit: ;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetNumLocalVariables( 
    /* [out] */ ULONG32 *numLocals)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (!m_methodDesc)
        {
            status = E_NOINTERFACE;
        }
        else
        {
            MetaSig* sig;

            status = GetLocalSig(&sig, numLocals);
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetLocalVariableByIndex( 
    /* [in] */ ULONG32 index,
    /* [out] */ IXCLRDataValue **localVariable,
    /* [in] */ ULONG32 bufLen,
    /* [out] */ ULONG32 *nameLen,
    /* [size_is][out] */ WCHAR name[  ])
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (!m_methodDesc)
        {
            status = E_NOINTERFACE;
            goto Exit;
        }

        MetaSig* sig;
        ULONG32 numLocals;
        
        if (FAILED(status = GetLocalSig(&sig, &numLocals)))
        {
            goto Exit;
        }

        if (index >= numLocals)
        {
            status = E_INVALIDARG;
            goto Exit;
        }

        MetaSig* argSig;
        ULONG32 numArgs;
        
        if (FAILED(status = GetMethodSig(&argSig, &numArgs)))
        {
            goto Exit;
        }

        if (bufLen && name)
        {
            if (nameLen)
            {
                *nameLen = 1;
            }

            name[0] = 0;
        }

        // The locals are indexed immediately following the arguments
        // in the NativeVarInfos.
        status = ValueFromDebugInfo(sig, false, index, index + numArgs,
                                    localVariable);

    Exit: ;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetNumTypeArguments( 
    /* [out] */ ULONG32 *numTypeArgs)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        // XXX drewb.
        status = E_NOTIMPL;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
        
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetTypeArgumentByIndex( 
    /* [in] */ ULONG32 index,
    /* [out] */ IXCLRDataTypeInstance **typeArg)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        // XXX drewb.
        status = E_NOTIMPL;
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}
        
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetCodeName( 
    /* [in] */ ULONG32 flags,
    /* [in] */ ULONG32 bufLen,
    /* [out] */ ULONG32 *symbolLen,
    /* [size_is][out] */ WCHAR symbolBuf[  ])
{
    HRESULT status = E_FAIL;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        TADDR pcAddr = taGetControlPC(&m_regDisp);
        status = m_dac->
            RawGetMethodName(TO_CDADDR(pcAddr), flags,
                             bufLen, symbolLen, symbolBuf,
                             NULL);
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();

    return status;
}
    
HRESULT STDMETHODCALLTYPE
ClrDataFrame::GetMethodInstance( 
    /* [out] */ IXCLRDataMethodInstance **method)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        if (!m_methodDesc)
        {
            status = E_NOINTERFACE;
        }
        else
        {
            ClrDataMethodInstance* dataMethod =
                new (nothrow) ClrDataMethodInstance(m_dac,
                                                    m_appDomain,
                                                    m_methodDesc);
            *method = static_cast<IXCLRDataMethodInstance*>(dataMethod);
            status = dataMethod ? S_OK : E_OUTOFMEMORY;
        }
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}

HRESULT STDMETHODCALLTYPE
ClrDataFrame::Request( 
    /* [in] */ ULONG32 reqCode,
    /* [in] */ ULONG32 inBufferSize,
    /* [size_is][in] */ BYTE *inBuffer,
    /* [in] */ ULONG32 outBufferSize,
    /* [size_is][out] */ BYTE *outBuffer)
{
    HRESULT status;

    DAC_ENTER_SUB(m_dac);
    
    EX_TRY
    {
        switch(reqCode)
        {
        case CLRDATA_REQUEST_REVISION:
            if (inBufferSize != 0 ||
                inBuffer ||
                outBufferSize != sizeof(ULONG32))
            {
                status = E_INVALIDARG;
            }
            else
            {
                *(ULONG32*)outBuffer = 1;
                status = S_OK;
            }
            break;

        default:
            status = E_INVALIDARG;
            break;
        }        
    }
    EX_CATCH
    {
        if (!DacExceptionFilter(GET_EXCEPTION(), m_dac, &status))
        {
            EX_RETHROW;
        }
    }
    EX_END_CATCH(SwallowAllExceptions)

    DAC_LEAVE();
    return status;
}

HRESULT
ClrDataFrame::GetMethodSig(MetaSig** sig,
                           ULONG32* count)
{
    if (!m_methodSig)
    {
        m_methodSig = new (nothrow) MetaSig(m_methodDesc);
        if (!m_methodSig)
        {
            return E_OUTOFMEMORY;
        }
    }

    *sig = m_methodSig;
    *count = m_methodSig->NumFixedArgs() +
        (m_methodSig->HasThis() ? 1 : 0);
    return *count ? S_OK : S_FALSE;
}

HRESULT
ClrDataFrame::GetLocalSig(MetaSig** sig,
                          ULONG32* count)
{
    if (!m_localSig)
    {
        // It turns out we cannot really get rid of this check.  Dynamic methods 
        // (including IL stubs) do not have their local sig's available after JIT time.
        if (!m_methodDesc->IsIL())
        {
            *sig = NULL;
            *count = 0;
            return S_FALSE;
        }

        COR_ILMETHOD_DECODER methodDecoder(m_methodDesc->GetILHeader());
        mdSignature localSig = methodDecoder.GetLocalVarSigTok() ?
            methodDecoder.GetLocalVarSigTok() : mdSignatureNil;
        if (localSig == mdSignatureNil)
        {
            *sig = NULL;
            *count = 0;
            return S_FALSE;
        }

        ULONG tokenSigLen;
        PCCOR_SIGNATURE tokenSig = m_methodDesc->GetModule()->GetMDImport()->
            GetSigFromToken(localSig, &tokenSigLen);

        SigTypeContext typeContext(m_methodDesc, TypeHandle());
        m_localSig = new (nothrow)
            MetaSig(tokenSig,
                    tokenSigLen,
                    m_methodDesc->GetModule(),
                    &typeContext,
                    FALSE,
                    MetaSig::sigLocalVars);
        if (!m_localSig)
        {
            return E_OUTOFMEMORY;
        }
    }

    *sig = m_localSig;
    *count = m_localSig->NumFixedArgs();
    return S_OK;
}

HRESULT
ClrDataFrame::ValueFromDebugInfo(MetaSig* sig,
                                 bool isArg,
                                 ULONG32 sigIndex,
                                 ULONG32 varInfoSlot,
                                 IXCLRDataValue** _value)
{
    HRESULT status;
    ULONG32 numVarInfo;
    NewHolder<ICorDebugInfo::NativeVarInfo> varInfo(NULL);
    ULONG32 codeOffset;
    ULONG32 valueFlags;
    ULONG32 i;

    if ((status = m_dac->GetMethodVarInfo(m_methodDesc,
                                          taGetControlPC(&m_regDisp),
                                          &numVarInfo,
                                          &varInfo,
                                          &codeOffset)) != S_OK)
    {
        // We have signature info indicating that there
        // are values, but couldn't find any location info.
        // Optimized routines may have eliminated all
        // traditional variable locations, so just treat
        // this as a no-location case just like not being
        // able to find a matching lifetime.
        numVarInfo = 0;
    }

    for (i = 0; i < numVarInfo; i++)
    {
        if (varInfo[i].startOffset <= codeOffset &&
            varInfo[i].endOffset >= codeOffset &&
            varInfo[i].varNumber == varInfoSlot &&
            varInfo[i].loc.vlType != ICorDebugInfo::VLT_INVALID)
        {
            break;
        }
    }

    ULONG64 baseAddr;
    NativeVarLocation locs[MAX_NATIVE_VAR_LOCS];
    ULONG32 numLocs;

    if (i >= numVarInfo)
    {
        numLocs = 0;
    }
    else
    {
        numLocs = NativeVarLocations(varInfo[i].loc, &m_context,
                                     NumItems(locs), locs);
    }

    if (numLocs == 1 && !locs[0].contextReg)
    {
        baseAddr = TO_CDADDR(locs[0].addr);
    }
    else
    {
        baseAddr = 0;
    }

    TypeHandle argType;

    sig->Reset();
    if (isArg && sigIndex == 0 && sig->HasThis())
    {
        argType = TypeHandle(m_methodDesc->GetMethodTable());
        valueFlags = CLRDATA_VALUE_IS_REFERENCE;
    }
    else
    {
        // 'this' doesn't show up in the signature but
        // is present in the indexing so adjust the
        // index down for methods with 'this'.
        if (isArg && sig->HasThis())
        {

            sigIndex--;
        }

        do
        {
            sig->NextArg();
        }
        while (sigIndex-- > 0);

        argType = sig->GetLastTypeHandleThrowing(ClassLoader::DontLoadTypes);
        if (argType.IsNull())
        {
            // XXX drewb - Sometimes types can't be looked
            // up and this at least allows the value to be used,
            // but is it the right behavior?
            argType = TypeHandle((&g_Mscorlib)->FetchClass(CLASS__UINT64, FALSE));
            valueFlags = 0;
        }
        else
        {
            valueFlags = GetTypeFieldValueFlags(argType, NULL, 0, false);

            // If this is a primitive variable and the actual size is smaller than what we have been told,
            // then lower the size so that we won't read in trash memory (e.g. reading 4 bytes for a short).
            if ((valueFlags & CLRDATA_VALUE_IS_PRIMITIVE) != 0)
            {
                if (numLocs == 1)
                {
                    UINT actualSize = argType.GetSize();
                    if (actualSize < locs[0].size)
                    {
                        locs[0].size = actualSize;
                    }
                }
            }
        }
    }

    ClrDataValue* value = new (nothrow)
        ClrDataValue(m_dac,
                     m_appDomain,
                     NULL,
                     valueFlags,
                     argType,
                     baseAddr,
                     numLocs,
                     locs);
    if (!value)
    {
        return E_OUTOFMEMORY;
    }

    *_value = value;
    return S_OK;
}
