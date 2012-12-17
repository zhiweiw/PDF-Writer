/*
   Source File : DocumentContext.cpp


   Copyright 2011 Gal Kahana PDFWriter

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   
*/
#include "DocumentContext.h"
#include "ObjectsContext.h"
#include "IByteWriterWithPosition.h"
#include "DictionaryContext.h"
#include "PDFPage.h"
#include "PageTree.h"
#include "BoxingBase.h"
#include "InfoDictionary.h"
#include "MD5Generator.h"
#include "OutputFile.h"
#include "Trace.h"
#include "IDocumentContextExtender.h"
#include "PageContentContext.h"
#include "PDFFormXObject.h"
#include "PDFParser.h"
#include "PDFObjectCast.h"
#include "PDFDictionary.h"
#include "PDFIndirectObjectReference.h"
#include "PDFInteger.h"
#include "PDFLiteralString.h"
#include "PDFBoolean.h"
#include "PDFArray.h"
#include "PDFDocumentCopyingContext.h"
#include "Ascii7Encoding.h"
#include "PDFHexString.h"
#include "PDFName.h"
#include "IResourceWritingTask.h"
#include "IFormEndWritingTask.h"

using namespace PDFHummus;

DocumentContext::DocumentContext()
{
	mObjectsContext = NULL;
	mParserExtender = NULL;
    mModifiedDocumentIDExists = false;
}

DocumentContext::~DocumentContext(void)
{
    Cleanup();
}

void DocumentContext::SetObjectsContext(ObjectsContext* inObjectsContext)
{
	mObjectsContext = inObjectsContext;
	mJPEGImageHandler.SetOperationsContexts(this,mObjectsContext);
	mTIFFImageHandler.SetOperationsContexts(this,mObjectsContext);
	mPDFDocumentHandler.SetOperationsContexts(this,mObjectsContext);
	mUsedFontsRepository.SetObjectsContext(mObjectsContext);
}

void DocumentContext::SetOutputFileInformation(OutputFile* inOutputFile)
{
	// just save the output file path for the ID generation in the end
	mOutputFilePath = inOutputFile->GetFilePath();
    mModifiedDocumentIDExists = false;
}

void DocumentContext::AddDocumentContextExtender(IDocumentContextExtender* inExtender)
{
	mExtenders.insert(inExtender);
	mJPEGImageHandler.AddDocumentContextExtender(inExtender);
	mPDFDocumentHandler.AddDocumentContextExtender(inExtender);

	PDFDocumentCopyingContextSet::iterator it = mCopyingContexts.begin();
	for(; it != mCopyingContexts.end(); ++it)
		(*it)->AddDocumentContextExtender(inExtender);
}

void DocumentContext::RemoveDocumentContextExtender(IDocumentContextExtender* inExtender)
{
	mExtenders.erase(inExtender);
	mJPEGImageHandler.RemoveDocumentContextExtender(inExtender);
	mPDFDocumentHandler.RemoveDocumentContextExtender(inExtender);
	PDFDocumentCopyingContextSet::iterator it = mCopyingContexts.begin();
	for(; it != mCopyingContexts.end(); ++it)
		(*it)->RemoveDocumentContextExtender(inExtender);
}

TrailerInformation& DocumentContext::GetTrailerInformation()
{
	return mTrailerInformation;
}

EStatusCode	DocumentContext::WriteHeader(EPDFVersion inPDFVersion)
{
	if(mObjectsContext)
	{
		WriteHeaderComment(inPDFVersion);
		Write4BinaryBytes();
		return PDFHummus::eSuccess;
	}
	else
		return PDFHummus::eFailure;
}

static const string scPDFVersion10 = "PDF-1.0";
static const string scPDFVersion11 = "PDF-1.1";
static const string scPDFVersion12 = "PDF-1.2";
static const string scPDFVersion13 = "PDF-1.3";
static const string scPDFVersion14 = "PDF-1.4";
static const string scPDFVersion15 = "PDF-1.5";
static const string scPDFVersion16 = "PDF-1.6";
static const string scPDFVersion17 = "PDF-1.7";

void DocumentContext::WriteHeaderComment(EPDFVersion inPDFVersion)
{
	switch(inPDFVersion)
	{
		case ePDFVersion10:
			mObjectsContext->WriteComment(scPDFVersion10);
			break;
		case ePDFVersion11:
			mObjectsContext->WriteComment(scPDFVersion11);
			break;
		case ePDFVersion12:
			mObjectsContext->WriteComment(scPDFVersion12);
			break;
		case ePDFVersion13:
			mObjectsContext->WriteComment(scPDFVersion13);
			break;
		case ePDFVersion14:
			mObjectsContext->WriteComment(scPDFVersion14);
			break;
		case ePDFVersion15:
			mObjectsContext->WriteComment(scPDFVersion15);
			break;
		case ePDFVersion16:
			mObjectsContext->WriteComment(scPDFVersion16);
			break;
		case ePDFVersion17:
        case ePDFVersionExtended:
			mObjectsContext->WriteComment(scPDFVersion17);
			break;
	}
}

static const IOBasicTypes::Byte scBinaryBytesArray[] = {'%',0xBD,0xBE,0xBC,'\r','\n'}; // might imply that i need a newline writer here....an underlying primitives-token context

void DocumentContext::Write4BinaryBytes()
{
	IByteWriterWithPosition *freeContextOutput = mObjectsContext->StartFreeContext();
	freeContextOutput->Write(scBinaryBytesArray,6);
	mObjectsContext->EndFreeContext();
}

EStatusCode	DocumentContext::FinalizeNewPDF()
{
	EStatusCode status;
	LongFilePositionType xrefTablePosition;


	// this will finalize writing all renments of the file, like xref, trailer and whatever objects still accumulating
	do
	{
		status = WriteUsedFontsDefinitions();
		if(status != 0)
			break;

		WritePagesTree();

		status = WriteCatalogObjectOfNewPDF();
		if(status != 0)
			break;


		// write the info dictionary of the trailer, if has any valid entries
		WriteInfoDictionary();

		status = mObjectsContext->WriteXrefTable(xrefTablePosition);
		if(status != 0)
			break;

		status = WriteTrailerDictionary();
		if(status != 0)
			break;

		WriteXrefReference(xrefTablePosition);
		WriteFinalEOF();
		
	} while(false);

	return status;
}


static const string scStartXref = "startxref";
void DocumentContext::WriteXrefReference(LongFilePositionType inXrefTablePosition)
{
	mObjectsContext->WriteKeyword(scStartXref);
	mObjectsContext->WriteInteger(inXrefTablePosition,eTokenSeparatorEndLine);
}

static const IOBasicTypes::Byte scEOF[] = {'%','%','E','O','F'}; 

void DocumentContext::WriteFinalEOF()
{
	IByteWriterWithPosition *freeContextOutput = mObjectsContext->StartFreeContext();
	freeContextOutput->Write(scEOF,5);
	mObjectsContext->EndFreeContext();
}

static const string scTrailer = "trailer";
static const string scSize = "Size";
static const string scPrev = "Prev";
static const string scRoot = "Root";
static const string scEncrypt = "Encrypt";
static const string scInfo = "Info";
static const string scID = "ID";
EStatusCode DocumentContext::WriteTrailerDictionary()
{
	DictionaryContext* dictionaryContext;
    
	mObjectsContext->WriteKeyword(scTrailer);
	dictionaryContext = mObjectsContext->StartDictionary();

    EStatusCode status = WriteTrailerDictionaryValues(dictionaryContext);
    
    mObjectsContext->EndDictionary(dictionaryContext);

    return status;
}

EStatusCode DocumentContext::WriteTrailerDictionaryValues(DictionaryContext* inDictionaryContext)
{
    EStatusCode status = eSuccess;

	do
	{
	
		// size
		inDictionaryContext->WriteKey(scSize);
		inDictionaryContext->WriteIntegerValue(mObjectsContext->GetInDirectObjectsRegistry().GetObjectsCount());

		// prev reference
		BoolAndLongFilePositionType filePositionResult = mTrailerInformation.GetPrev(); 
		if(filePositionResult.first)
		{
			inDictionaryContext->WriteKey(scPrev);
			inDictionaryContext->WriteIntegerValue(filePositionResult.second);
		}

		// catalog reference
		BoolAndObjectReference objectIDResult = mTrailerInformation.GetRoot();
		if(objectIDResult.first)
		{
			inDictionaryContext->WriteKey(scRoot);
			inDictionaryContext->WriteObjectReferenceValue(objectIDResult.second);
		}
		else
		{
			TRACE_LOG("DocumentContext::WriteTrailerDictionaryValues, Unexpected Failure. Didn't find catalog object while writing trailer");
			status = PDFHummus::eFailure;
			break;
		}

		// encrypt dictionary reference
		objectIDResult = mTrailerInformation.GetEncrypt();
		if(objectIDResult.first)
		{
			inDictionaryContext->WriteKey(scEncrypt);
			inDictionaryContext->WriteObjectReferenceValue(objectIDResult.second);
		}

		// info reference
		objectIDResult = mTrailerInformation.GetInfoDictionaryReference();
		if(objectIDResult.first)
		{
			inDictionaryContext->WriteKey(scInfo);
			inDictionaryContext->WriteObjectReferenceValue(objectIDResult.second);
		}

		// write ID

		string id = GenerateMD5IDForFile();
		inDictionaryContext->WriteKey(scID);
		mObjectsContext->StartArray();
        
        // if modified file scenario use original ID, otherwise create a new one for the document created ID
        if(mModifiedDocumentIDExists)
            mObjectsContext->WriteHexString(mModifiedDocumentID);
        else
            mObjectsContext->WriteHexString(id);
		mObjectsContext->WriteHexString(id);
		mObjectsContext->EndArray();
		mObjectsContext->EndLine();

	}while(false);
	
	return status;
}

static const string scTitle = "Title";
static const string scAuthor = "Author";
static const string scSubject = "Subject";
static const string scKeywords = "Keywords";
static const string scCreator = "Creator";
static const string scProducer = "Producer";
static const string scCreationDate = "CreationDate";
static const string scModDate = "ModDate";
static const string scTrapped = "Trapped";
static const string scTrue = "True";
static const string scFalse = "False";

void DocumentContext::WriteInfoDictionary()
{
	InfoDictionary& infoDictionary = mTrailerInformation.GetInfo();
	if(infoDictionary.IsEmpty())
		return;

	ObjectIDType infoDictionaryID = mObjectsContext->StartNewIndirectObject();
	DictionaryContext* infoContext = mObjectsContext->StartDictionary();

	mTrailerInformation.SetInfoDictionaryReference(infoDictionaryID);

	if(!infoDictionary.Title.IsEmpty()) 
	{
		infoContext->WriteKey(scTitle);
		infoContext->WriteLiteralStringValue(infoDictionary.Title.ToString());
	}
	
	if(!infoDictionary.Author.IsEmpty()) 
	{
		infoContext->WriteKey(scAuthor);
		infoContext->WriteLiteralStringValue(infoDictionary.Author.ToString());
	}

	if(!infoDictionary.Subject.IsEmpty()) 
	{
		infoContext->WriteKey(scSubject);
		infoContext->WriteLiteralStringValue(infoDictionary.Subject.ToString());
	}

	if(!infoDictionary.Keywords.IsEmpty()) 
	{
		infoContext->WriteKey(scKeywords);
		infoContext->WriteLiteralStringValue(infoDictionary.Keywords.ToString());
	}

	if(!infoDictionary.Creator.IsEmpty()) 
	{
		infoContext->WriteKey(scCreator);
		infoContext->WriteLiteralStringValue(infoDictionary.Creator.ToString());
	}

	if(!infoDictionary.Producer.IsEmpty()) 
	{
		infoContext->WriteKey(scProducer);
		infoContext->WriteLiteralStringValue(infoDictionary.Producer.ToString());
	}

	if(!infoDictionary.CreationDate.IsNull()) 
	{
		infoContext->WriteKey(scCreationDate);
		infoContext->WriteLiteralStringValue(infoDictionary.CreationDate.ToString());
	}

	if(!infoDictionary.ModDate.IsNull()) 
	{
		infoContext->WriteKey(scModDate);
		infoContext->WriteLiteralStringValue(infoDictionary.ModDate.ToString());
	}

	if(EInfoTrappedUnknown != infoDictionary.Trapped)
	{
		infoContext->WriteKey(scTrapped);
		infoContext->WriteNameValue(EInfoTrappedTrue == infoDictionary.Trapped ? scTrue : scFalse);
	}

	MapIterator<StringToPDFTextString> it = infoDictionary.GetAdditionaEntriesIterator();

	while(it.MoveNext())
	{
		infoContext->WriteKey(it.GetKey());
		infoContext->WriteLiteralStringValue(it.GetValue().ToString());
	}

	mObjectsContext->EndDictionary(infoContext);
	mObjectsContext->EndIndirectObject();
	
}

CatalogInformation& DocumentContext::GetCatalogInformation()
{
	return mCatalogInformation;
}

static const string scType = "Type";
static const string scCatalog = "Catalog";
static const string scPages = "Pages";
EStatusCode DocumentContext::WriteCatalogObjectOfNewPDF()
{
    return WriteCatalogObject(mCatalogInformation.GetPageTreeRoot(mObjectsContext->GetInDirectObjectsRegistry())->GetID());
    
}

EStatusCode DocumentContext::WriteCatalogObject(const ObjectReference& inPageTreeRootObjectReference)
{
	EStatusCode status = PDFHummus::eSuccess;
	ObjectIDType catalogID = mObjectsContext->StartNewIndirectObject();
	mTrailerInformation.SetRoot(catalogID); // set the catalog reference as root in the trailer

	DictionaryContext* catalogContext = mObjectsContext->StartDictionary();

	catalogContext->WriteKey(scType);
	catalogContext->WriteNameValue(scCatalog);

	catalogContext->WriteKey(scPages);
	catalogContext->WriteObjectReferenceValue(inPageTreeRootObjectReference);

	IDocumentContextExtenderSet::iterator it = mExtenders.begin();
	for(; it != mExtenders.end() && PDFHummus::eSuccess == status; ++it)
	{
		status = (*it)->OnCatalogWrite(&mCatalogInformation,catalogContext,mObjectsContext,this);
		if(status != PDFHummus::eSuccess)
			TRACE_LOG("DocumentContext::WriteCatalogObject, unexpected failure. extender declared failure when writing catalog.");
	}

	mObjectsContext->EndDictionary(catalogContext);
	mObjectsContext->EndIndirectObject();
	return status;
}


void DocumentContext::WritePagesTree()
{
	PageTree* pageTreeRoot = mCatalogInformation.GetPageTreeRoot(mObjectsContext->GetInDirectObjectsRegistry());

	WritePageTree(pageTreeRoot);
}

static const string scCount = "Count";
static const string scKids = "Kids";
static const string scParent = "Parent";

// Recursion to write a page tree node. the return result is the page nodes count, for
// accumulation at higher levels
int DocumentContext::WritePageTree(PageTree* inPageTreeToWrite)
{
	DictionaryContext* pagesTreeContext;

	if(inPageTreeToWrite->IsLeafParent())
	{
		mObjectsContext->StartNewIndirectObject(inPageTreeToWrite->GetID());

		pagesTreeContext = mObjectsContext->StartDictionary();

		// type
		pagesTreeContext->WriteKey(scType);
		pagesTreeContext->WriteNameValue(scPages);
		
		// count
		pagesTreeContext->WriteKey(scCount);
		pagesTreeContext->WriteIntegerValue(inPageTreeToWrite->GetNodesCount());

		// kids
		pagesTreeContext->WriteKey(scKids);
		mObjectsContext->StartArray();
		for(int i=0;i<inPageTreeToWrite->GetNodesCount();++i)
			mObjectsContext->WriteNewIndirectObjectReference(inPageTreeToWrite->GetPageIDChild(i));
		mObjectsContext->EndArray();
		mObjectsContext->EndLine();

		// parent
		if(inPageTreeToWrite->GetParent())
		{
			pagesTreeContext->WriteKey(scParent);
			pagesTreeContext->WriteNewObjectReferenceValue(inPageTreeToWrite->GetParent()->GetID());
		}

		mObjectsContext->EndDictionary(pagesTreeContext);
		mObjectsContext->EndIndirectObject();

		return inPageTreeToWrite->GetNodesCount();
	}
	else
	{
		int totalPagesNodes = 0;

		// first loop the kids and write them (while at it, accumulate the children count).
		for(int i=0;i<inPageTreeToWrite->GetNodesCount();++i)
			totalPagesNodes += WritePageTree(inPageTreeToWrite->GetPageTreeChild(i));

		mObjectsContext->StartNewIndirectObject(inPageTreeToWrite->GetID());

		pagesTreeContext = mObjectsContext->StartDictionary();

		// type
		pagesTreeContext->WriteKey(scType);
		pagesTreeContext->WriteNameValue(scPages);
		
		// count
		pagesTreeContext->WriteKey(scCount);
		pagesTreeContext->WriteIntegerValue(totalPagesNodes);

		// kids
		pagesTreeContext->WriteKey(scKids);
		mObjectsContext->StartArray();
		for(int j=0;j<inPageTreeToWrite->GetNodesCount();++j)
			mObjectsContext->WriteNewIndirectObjectReference(inPageTreeToWrite->GetPageTreeChild(j)->GetID());
		mObjectsContext->EndArray();
		mObjectsContext->EndLine();

		// parent
		if(inPageTreeToWrite->GetParent())
		{
			pagesTreeContext->WriteKey(scParent);
			pagesTreeContext->WriteNewObjectReferenceValue(inPageTreeToWrite->GetParent()->GetID());
		}

		mObjectsContext->EndDictionary(pagesTreeContext);
		mObjectsContext->EndIndirectObject();

		return totalPagesNodes;
	}
}

static const string scResources = "Resources";
static const string scPage = "Page";
static const string scMediaBox = "MediaBox";
static const string scCropBox = "CropBox";
static const string scBleedBox = "BleedBox";
static const string scTrimBox = "TrimBox";
static const string scArtBox = "ArtBox";
static const string scContents = "Contents";

EStatusCodeAndObjectIDType DocumentContext::WritePage(PDFPage* inPage)
{
	EStatusCodeAndObjectIDType result;
	
	result.first = PDFHummus::eSuccess;
	result.second = mObjectsContext->StartNewIndirectObject();

	DictionaryContext* pageContext = mObjectsContext->StartDictionary();

	// type
	pageContext->WriteKey(scType);
	pageContext->WriteNameValue(scPage);

	// parent
	pageContext->WriteKey(scParent);
	pageContext->WriteNewObjectReferenceValue(mCatalogInformation.AddPageToPageTree(result.second,mObjectsContext->GetInDirectObjectsRegistry()));
	
	// Media Box
	pageContext->WriteKey(scMediaBox);
	pageContext->WriteRectangleValue(inPage->GetMediaBox());

    // Crop Box
    PDFRectangle cropBox;
    if(inPage->GetCropBox().first && (inPage->GetCropBox().second != inPage->GetMediaBox()))
    {
        pageContext->WriteKey(scCropBox);
        pageContext->WriteRectangleValue(inPage->GetCropBox().second);
        cropBox = inPage->GetCropBox().second;
    }
    else 
        cropBox = inPage->GetMediaBox();
    
    
    // Bleed Box
    if(inPage->GetBleedBox().first && (inPage->GetBleedBox().second != cropBox))
    {
        pageContext->WriteKey(scBleedBox);
        pageContext->WriteRectangleValue(inPage->GetBleedBox().second);
    }
    
    // Trim Box
    if(inPage->GetTrimBox().first && (inPage->GetTrimBox().second != cropBox))
    {
        pageContext->WriteKey(scTrimBox);
        pageContext->WriteRectangleValue(inPage->GetTrimBox().second);
    }    
    
    // Art Box
    if(inPage->GetArtBox().first && (inPage->GetArtBox().second != cropBox))
    {
        pageContext->WriteKey(scArtBox);
        pageContext->WriteRectangleValue(inPage->GetArtBox().second);
    }    
    
    
	do
	{
		// Resource dict 
		pageContext->WriteKey(scResources);
		result.first = WriteResourcesDictionary(inPage->GetResourcesDictionary());
		if(result.first != PDFHummus::eSuccess)
		{
			TRACE_LOG("DocumentContext::WritePage, failed to write resources dictionary");
			break;
		}

		// Annotations
		if(mAnnotations.size() > 0)
		{
			pageContext->WriteKey("Annots");

			ObjectIDTypeSet::iterator it = mAnnotations.begin();

			mObjectsContext->StartArray();
			for(; it != mAnnotations.end(); ++it)
				mObjectsContext->WriteNewIndirectObjectReference(*it);
			mObjectsContext->EndArray(eTokenSeparatorEndLine);			
		}
		mAnnotations.clear();

		// Content
		if(inPage->GetContentStreamsCount() > 0)
		{
			SingleValueContainerIterator<ObjectIDTypeList> iterator = inPage->GetContentStreamReferencesIterator();

			pageContext->WriteKey(scContents);
			if(inPage->GetContentStreamsCount() > 1)
			{
				mObjectsContext->StartArray();
				while(iterator.MoveNext())
					mObjectsContext->WriteNewIndirectObjectReference(iterator.GetItem());
				mObjectsContext->EndArray();	
				mObjectsContext->EndLine();
			}
			else
			{
				iterator.MoveNext();
				pageContext->WriteNewObjectReferenceValue(iterator.GetItem());
			}
		}

		IDocumentContextExtenderSet::iterator it = mExtenders.begin();
		for(; it != mExtenders.end() && PDFHummus::eSuccess == result.first; ++it)
		{
			result.first = (*it)->OnPageWrite(inPage,pageContext,mObjectsContext,this);
			if(result.first != PDFHummus::eSuccess)
			{
				TRACE_LOG("DocumentContext::WritePage, unexpected failure. extender declared failure when writing page.");
				break;
			}
		}
		result.first = mObjectsContext->EndDictionary(pageContext);
		if(result.first != PDFHummus::eSuccess)
		{
			TRACE_LOG("DocumentContext::WritePage, unexpected failure. Failed to end dictionary in page write.");
			break;
		}
		mObjectsContext->EndIndirectObject();	
	}while(false);

	return result;
}


EStatusCodeAndObjectIDType DocumentContext::WritePageAndRelease(PDFPage* inPage)
{
	EStatusCodeAndObjectIDType status = WritePage(inPage);
	delete inPage;
	return status;
}

static const string scUnknown = "Unknown";
string DocumentContext::GenerateMD5IDForFile()
{
	MD5Generator md5;

	// encode current time
	PDFDate currentTime;
	currentTime.SetToCurrentTime();
	md5.Accumulate(currentTime.ToString());

	// file location
	md5.Accumulate(mOutputFilePath);

	// current writing position (will serve as "file size")
	IByteWriterWithPosition *outputStream = mObjectsContext->StartFreeContext();
	mObjectsContext->EndFreeContext();

	md5.Accumulate(BoxingBaseWithRW<LongFilePositionType>(outputStream->GetCurrentPosition()).ToString());

	// document information dictionary
	InfoDictionary& infoDictionary = mTrailerInformation.GetInfo();

	md5.Accumulate(infoDictionary.Title.ToString());
	md5.Accumulate(infoDictionary.Author.ToString());
	md5.Accumulate(infoDictionary.Subject.ToString());
	md5.Accumulate(infoDictionary.Keywords.ToString());
	md5.Accumulate(infoDictionary.Creator.ToString());
	md5.Accumulate(infoDictionary.Producer.ToString());
	md5.Accumulate(infoDictionary.CreationDate.ToString());
	md5.Accumulate(infoDictionary.ModDate.ToString());
	md5.Accumulate(EInfoTrappedUnknown == infoDictionary.Trapped ? scUnknown:(EInfoTrappedTrue == infoDictionary.Trapped ? scTrue:scFalse));

	MapIterator<StringToPDFTextString> it = infoDictionary.GetAdditionaEntriesIterator();

	while(it.MoveNext())
		md5.Accumulate(it.GetValue().ToString());

	return md5.ToString();
}

bool DocumentContext::HasContentContext(PDFPage* inPage)
{
	return inPage->GetAssociatedContentContext() != NULL;
}

PageContentContext* DocumentContext::StartPageContentContext(PDFPage* inPage)
{
	if(!inPage->GetAssociatedContentContext())
	{
		inPage->AssociateContentContext(new PageContentContext(inPage,mObjectsContext));
	}
	return inPage->GetAssociatedContentContext();
}

EStatusCode DocumentContext::PausePageContentContext(PageContentContext* inPageContext)
{
	return inPageContext->FinalizeCurrentStream();
}

EStatusCode DocumentContext::EndPageContentContext(PageContentContext* inPageContext)
{
	EStatusCode status = inPageContext->FinalizeCurrentStream();
	inPageContext->GetAssociatedPage()->DisassociateContentContext();
	delete inPageContext;
	return status;
}

static const string scXObject = "XObject";
static const string scSubType = "Subtype";
static const string scForm = "Form";
static const string scBBox = "BBox";
static const string scFormType = "FormType";
static const string scMatrix = "Matrix";
PDFFormXObject* DocumentContext::StartFormXObject(const PDFRectangle& inBoundingBox,ObjectIDType inFormXObjectID,const double* inMatrix)
{
	PDFFormXObject* aFormXObject = NULL;
	do
	{
		mObjectsContext->StartNewIndirectObject(inFormXObjectID);
		DictionaryContext* xobjectContext = mObjectsContext->StartDictionary();

		// type
		xobjectContext->WriteKey(scType);
		xobjectContext->WriteNameValue(scXObject);

		// subtype
		xobjectContext->WriteKey(scSubType);
		xobjectContext->WriteNameValue(scForm);

		// form type
		xobjectContext->WriteKey(scFormType);
		xobjectContext->WriteIntegerValue(1);

		// bbox
		xobjectContext->WriteKey(scBBox);
		xobjectContext->WriteRectangleValue(inBoundingBox);

		// matrix
		if(inMatrix && !IsIdentityMatrix(inMatrix))
		{
			xobjectContext->WriteKey(scMatrix);
			mObjectsContext->StartArray();
			for(int i=0;i<6;++i)
				mObjectsContext->WriteDouble(inMatrix[i]);
			mObjectsContext->EndArray(eTokenSeparatorEndLine);
		}

		// Resource dict 
		xobjectContext->WriteKey(scResources);	
		// put a resources dictionary place holder
		ObjectIDType formXObjectResourcesDictionaryID = mObjectsContext->GetInDirectObjectsRegistry().AllocateNewObjectID();
		xobjectContext->WriteNewObjectReferenceValue(formXObjectResourcesDictionaryID);

		IDocumentContextExtenderSet::iterator it = mExtenders.begin();
		EStatusCode status = PDFHummus::eSuccess;
		for(; it != mExtenders.end() && PDFHummus::eSuccess == status; ++it)
		{
			if((*it)->OnFormXObjectWrite(inFormXObjectID,formXObjectResourcesDictionaryID,xobjectContext,mObjectsContext,this) != PDFHummus::eSuccess)
			{
				TRACE_LOG("DocumentContext::StartFormXObject, unexpected failure. extender declared failure when writing form xobject.");
				status = PDFHummus::eFailure;
				break;
			}
		}
		if(status != PDFHummus::eSuccess)
			break;

		// Now start the stream and the form XObject state
		aFormXObject =  new PDFFormXObject(inFormXObjectID,mObjectsContext->StartPDFStream(xobjectContext),formXObjectResourcesDictionaryID);
	} while(false);

	return aFormXObject;	
}


PDFFormXObject* DocumentContext::StartFormXObject(const PDFRectangle& inBoundingBox,const double* inMatrix)
{
	ObjectIDType formXObjectID = mObjectsContext->GetInDirectObjectsRegistry().AllocateNewObjectID();
	return StartFormXObject(inBoundingBox,formXObjectID,inMatrix);
}

EStatusCode DocumentContext::EndFormXObjectNoRelease(PDFFormXObject* inFormXObject)
{
	mObjectsContext->EndPDFStream(inFormXObject->GetContentStream());

	// now write the resources dictionary, full of all the goodness that got accumulated over the stream write
	mObjectsContext->StartNewIndirectObject(inFormXObject->GetResourcesDictionaryObjectID());
	WriteResourcesDictionary(inFormXObject->GetResourcesDictionary());
	mObjectsContext->EndIndirectObject();
	
    // now write writing tasks
    PDFFormXObjectToIFormEndWritingTaskListMap::iterator it= mFormEndTasks.find(inFormXObject);
    
    EStatusCode status = eSuccess;
    if(it != mFormEndTasks.end())
    {
        IFormEndWritingTaskList::iterator itTasks = it->second.begin();
        
        for(; itTasks != it->second.end() && eSuccess == status; ++itTasks)
            status = (*itTasks)->Write(inFormXObject,mObjectsContext,this);
        
        // one time, so delete
        for(itTasks = it->second.begin(); itTasks != it->second.end(); ++itTasks)
            delete (*itTasks);
        mFormEndTasks.erase(it); 
    }
    
	return status;
}

EStatusCode DocumentContext::EndFormXObjectAndRelease(PDFFormXObject* inFormXObject)
{
	EStatusCode status = EndFormXObjectNoRelease(inFormXObject);
	delete inFormXObject; // will also delete the stream becuase the form XObject owns it
	
	return status;
}

static const string scProcesets = "ProcSet";
static const string scXObjects = "XObject";
static const string scExtGStates = "ExtGState";
static const string scFonts = "Font";
static const string scColorSpaces = "ColorSpace";
static const string scPatterns = "Pattern";
static const string scShadings = "Shading";
static const string scProperties = "Properties";
EStatusCode DocumentContext::WriteResourcesDictionary(ResourcesDictionary& inResourcesDictionary)
{
	EStatusCode status = PDFHummus::eSuccess;

	do
	{

		DictionaryContext* resourcesContext = mObjectsContext->StartDictionary();


        //	Procsets
        SingleValueContainerIterator<StringSet> itProcesets = inResourcesDictionary.GetProcesetsIterator();
        if(itProcesets.MoveNext())
        {
            resourcesContext->WriteKey(scProcesets);
            mObjectsContext->StartArray();
            do 
            {
                mObjectsContext->WriteName(itProcesets.GetItem());
            } 
            while (itProcesets.MoveNext());
            mObjectsContext->EndArray();
            mObjectsContext->EndLine();
        }

        // XObjects
        status = WriteResourceDictionary(&inResourcesDictionary,resourcesContext,scXObjects,inResourcesDictionary.GetXObjectsIterator());
        if(status!=eSuccess)
            break;

		// ExtGStates
        status = WriteResourceDictionary(&inResourcesDictionary,resourcesContext,scExtGStates,inResourcesDictionary.GetExtGStatesIterator());
        if(status!=eSuccess)
            break;

		// Fonts
        status = WriteResourceDictionary(&inResourcesDictionary,resourcesContext,scFonts,inResourcesDictionary.GetFontsIterator());
        if(status!=eSuccess)
            break;

		// Color space
        status = WriteResourceDictionary(&inResourcesDictionary,resourcesContext,scColorSpaces,inResourcesDictionary.GetColorSpacesIterator());
	
		// Patterns
        status = WriteResourceDictionary(&inResourcesDictionary,resourcesContext,scPatterns,inResourcesDictionary.GetPatternsIterator());
        if(status!=eSuccess)
            break;

		// Shading
        status = WriteResourceDictionary(&inResourcesDictionary,resourcesContext,scShadings,inResourcesDictionary.GetShadingsIterator());
        if(status!=eSuccess)
            break;

		// Properties
        status = WriteResourceDictionary(&inResourcesDictionary,resourcesContext,scProperties,inResourcesDictionary.GetPropertiesIterator());
        if(status!=eSuccess)
            break;

		IDocumentContextExtenderSet::iterator itExtenders = mExtenders.begin();
		for(; itExtenders != mExtenders.end() && PDFHummus::eSuccess == status; ++itExtenders)
		{
			status = (*itExtenders)->OnResourcesWrite(&(inResourcesDictionary),resourcesContext,mObjectsContext,this);
			if(status != PDFHummus::eSuccess)
			{
				TRACE_LOG("DocumentContext::WriteResourcesDictionary, unexpected failure. extender declared failure when writing resources.");
				break;
			}
		}

		mObjectsContext->EndDictionary(resourcesContext); 
	}while(false);

	return status;
}

EStatusCode DocumentContext::WriteResourceDictionary(ResourcesDictionary* inResourcesDictionary,
                                              DictionaryContext* inResourcesCategoryDictionary,
											const string& inResourceDictionaryLabel,
											MapIterator<ObjectIDTypeToStringMap> inMapping)
{
    EStatusCode status = eSuccess;
    
    ResourcesDictionaryAndStringToIResourceWritingTaskListMap::iterator itWriterTasks = 
        mResourcesTasks.find(ResourcesDictionaryAndString(inResourcesDictionary,inResourceDictionaryLabel));
    
    if(inMapping.MoveNext() || itWriterTasks != mResourcesTasks.end())
    {
        do {
            inResourcesCategoryDictionary->WriteKey(inResourceDictionaryLabel);
            DictionaryContext* resourceContext = mObjectsContext->StartDictionary();
            
            if(!inMapping.IsFinished())
            {
                do
                {
                    resourceContext->WriteKey(inMapping.GetValue());
                    resourceContext->WriteNewObjectReferenceValue(inMapping.GetKey());
                }
                while(inMapping.MoveNext());
            }
            
            if(itWriterTasks != mResourcesTasks.end())
            {
                IResourceWritingTaskList::iterator itTasks = itWriterTasks->second.begin();
                
                for(; itTasks != itWriterTasks->second.end() && eSuccess == status; ++itTasks)
                    status = (*itTasks)->Write(inResourcesCategoryDictionary,mObjectsContext,this);
                
                // Discard the tasks for this category
                for(itTasks = itWriterTasks->second.begin(); itTasks != itWriterTasks->second.end(); ++itTasks)
                    delete *itTasks;
                mResourcesTasks.erase(itWriterTasks);
                if(status != eSuccess)
                    break;
            }
            
            IDocumentContextExtenderSet::iterator it = mExtenders.begin();
            EStatusCode status = PDFHummus::eSuccess;
            for(; it != mExtenders.end() && eSuccess == status; ++it)
            {
                status = (*it)->OnResourceDictionaryWrite(resourceContext,inResourceDictionaryLabel,mObjectsContext,this);
                if(status != PDFHummus::eSuccess)
                {
                    TRACE_LOG("DocumentContext::WriteResourceDictionary, unexpected failure. extender declared failure when writing a resource dictionary.");
                    break;
                }
            }
            
            mObjectsContext->EndDictionary(resourceContext);
            
        } 
        while (false);

    }
    
    return status;
}



bool DocumentContext::IsIdentityMatrix(const double* inMatrix)
{
	return 
		inMatrix[0] == 1 &&
		inMatrix[1] == 0 &&
		inMatrix[2] == 0 &&
		inMatrix[3] == 1 &&
		inMatrix[4] == 0 &&
		inMatrix[5] == 0;

}

PDFImageXObject* DocumentContext::CreateImageXObjectFromJPGFile(const string& inJPGFilePath)
{
	return mJPEGImageHandler.CreateImageXObjectFromJPGFile(inJPGFilePath);
}

PDFFormXObject* DocumentContext::CreateFormXObjectFromJPGFile(const string& inJPGFilePath)
{
	return mJPEGImageHandler.CreateFormXObjectFromJPGFile(inJPGFilePath);
}

JPEGImageHandler& DocumentContext::GetJPEGImageHandler()
{
	return mJPEGImageHandler;
}

PDFFormXObject* DocumentContext::CreateFormXObjectFromTIFFFile(	const string& inTIFFFilePath,
																const TIFFUsageParameters& inTIFFUsageParameters)
{
	
	return mTIFFImageHandler.CreateFormXObjectFromTIFFFile(inTIFFFilePath,inTIFFUsageParameters);
}

PDFImageXObject* DocumentContext::CreateImageXObjectFromJPGFile(const string& inJPGFilePath,ObjectIDType inImageXObjectID)
{
	return mJPEGImageHandler.CreateImageXObjectFromJPGFile(inJPGFilePath,inImageXObjectID);
}

PDFFormXObject* DocumentContext::CreateFormXObjectFromJPGFile(const string& inJPGFilePath,ObjectIDType inFormXObjectID)
{
	return mJPEGImageHandler.CreateFormXObjectFromJPGFile(inJPGFilePath,inFormXObjectID);
}

PDFFormXObject* DocumentContext::CreateFormXObjectFromTIFFFile(	
												const string& inTIFFFilePath,
												ObjectIDType inFormXObjectID,
												const TIFFUsageParameters& inTIFFUsageParameters)
{
	return mTIFFImageHandler.CreateFormXObjectFromTIFFFile(inTIFFFilePath,inFormXObjectID,inTIFFUsageParameters);
}


PDFUsedFont* DocumentContext::GetFontForFile(const string& inFontFilePath)
{
	return mUsedFontsRepository.GetFontForFile(inFontFilePath);
}

EStatusCode DocumentContext::WriteUsedFontsDefinitions()
{
	return mUsedFontsRepository.WriteUsedFontsDefinitions();
}

PDFUsedFont* DocumentContext::GetFontForFile(const string& inFontFilePath,const string& inAdditionalMeticsFilePath)
{
	return mUsedFontsRepository.GetFontForFile(inFontFilePath,inAdditionalMeticsFilePath);
}

EStatusCodeAndObjectIDTypeList DocumentContext::CreateFormXObjectsFromPDF(const string& inPDFFilePath,
																			const PDFPageRange& inPageRange,
																			EPDFPageBox inPageBoxToUseAsFormBox,
																			const double* inTransformationMatrix,
																			const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.CreateFormXObjectsFromPDF(inPDFFilePath,inPageRange,inPageBoxToUseAsFormBox,inTransformationMatrix,inCopyAdditionalObjects);	

}

EStatusCodeAndObjectIDTypeList DocumentContext::CreateFormXObjectsFromPDF(const string& inPDFFilePath,
																			const PDFPageRange& inPageRange,
																			const PDFRectangle& inCropBox,
																			const double* inTransformationMatrix,
																			const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.CreateFormXObjectsFromPDF(inPDFFilePath,inPageRange,inCropBox,inTransformationMatrix,inCopyAdditionalObjects);	

}
EStatusCodeAndObjectIDTypeList DocumentContext::AppendPDFPagesFromPDF(const string& inPDFFilePath,
																	  const PDFPageRange& inPageRange,
																	  const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.AppendPDFPagesFromPDF(inPDFFilePath,inPageRange,inCopyAdditionalObjects);	
}

EStatusCode DocumentContext::WriteState(ObjectsContext* inStateWriter,ObjectIDType inObjectID)
{
	EStatusCode status;

	do
	{
		inStateWriter->StartNewIndirectObject(inObjectID);

		ObjectIDType trailerInformationID = inStateWriter->GetInDirectObjectsRegistry().AllocateNewObjectID();
		ObjectIDType catalogInformationID = inStateWriter->GetInDirectObjectsRegistry().AllocateNewObjectID();
		ObjectIDType usedFontsRepositoryID = inStateWriter->GetInDirectObjectsRegistry().AllocateNewObjectID();

		DictionaryContext* documentDictionary = inStateWriter->StartDictionary();

		documentDictionary->WriteKey("Type");
		documentDictionary->WriteNameValue("DocumentContext");

		documentDictionary->WriteKey("mTrailerInformation");
		documentDictionary->WriteNewObjectReferenceValue(trailerInformationID);

		documentDictionary->WriteKey("mCatalogInformation");
		documentDictionary->WriteNewObjectReferenceValue(catalogInformationID);

		documentDictionary->WriteKey("mUsedFontsRepository");
		documentDictionary->WriteNewObjectReferenceValue(usedFontsRepositoryID);
        
        documentDictionary->WriteKey("mModifiedDocumentIDExists");
        documentDictionary->WriteBooleanValue(mModifiedDocumentIDExists);
        
        if(mModifiedDocumentIDExists)
        {
            documentDictionary->WriteKey("mModifiedDocumentID");
            documentDictionary->WriteHexStringValue(mModifiedDocumentID);
        }

		inStateWriter->EndDictionary(documentDictionary);
		inStateWriter->EndIndirectObject();

		WriteTrailerState(inStateWriter,trailerInformationID);
		WriteCatalogInformationState(inStateWriter,catalogInformationID);
		
		status = mUsedFontsRepository.WriteState(inStateWriter,usedFontsRepositoryID);
		if(status != PDFHummus::eSuccess)
			break;
	}while(false);

	return status;
}

void DocumentContext::WriteReferenceState(ObjectsContext* inStateWriter,
                                          const ObjectReference& inReference)
{
    DictionaryContext* referenceContext = inStateWriter->StartDictionary();
    
    referenceContext->WriteKey("ObjectID");
    referenceContext->WriteIntegerValue(inReference.ObjectID);
    
    referenceContext->WriteKey("GenerationNumber");
    referenceContext->WriteIntegerValue(inReference.GenerationNumber);
    
    inStateWriter->EndDictionary(referenceContext);
}

void DocumentContext::WriteTrailerState(ObjectsContext* inStateWriter,ObjectIDType inObjectID)
{
	inStateWriter->StartNewIndirectObject(inObjectID);

	DictionaryContext* trailerDictionary = inStateWriter->StartDictionary();

	trailerDictionary->WriteKey("Type");
	trailerDictionary->WriteNameValue("TrailerInformation");

	trailerDictionary->WriteKey("mPrev");
	trailerDictionary->WriteIntegerValue(mTrailerInformation.GetPrev().second);

	trailerDictionary->WriteKey("mRootReference");
	WriteReferenceState(inStateWriter,mTrailerInformation.GetRoot().second);

	trailerDictionary->WriteKey("mEncryptReference");
	WriteReferenceState(inStateWriter,mTrailerInformation.GetEncrypt().second);

	trailerDictionary->WriteKey("mInfoDictionary");
	ObjectIDType infoDictionaryID = inStateWriter->GetInDirectObjectsRegistry().AllocateNewObjectID();
	trailerDictionary->WriteNewObjectReferenceValue(infoDictionaryID);

	trailerDictionary->WriteKey("mInfoDictionaryReference");
	WriteReferenceState(inStateWriter,mTrailerInformation.GetInfoDictionaryReference().second);

	inStateWriter->EndDictionary(trailerDictionary);
	inStateWriter->EndIndirectObject();

	WriteTrailerInfoState(inStateWriter,infoDictionaryID);
}

void DocumentContext::WriteTrailerInfoState(ObjectsContext* inStateWriter,ObjectIDType inObjectID)
{
	inStateWriter->StartNewIndirectObject(inObjectID);
	DictionaryContext* infoDictionary = inStateWriter->StartDictionary();

	infoDictionary->WriteKey("Type");
	infoDictionary->WriteNameValue("InfoDictionary");

	infoDictionary->WriteKey("Title");
	infoDictionary->WriteLiteralStringValue(mTrailerInformation.GetInfo().Title.ToString());

	infoDictionary->WriteKey("Author");
	infoDictionary->WriteLiteralStringValue(mTrailerInformation.GetInfo().Author.ToString());

	infoDictionary->WriteKey("Subject");
	infoDictionary->WriteLiteralStringValue(mTrailerInformation.GetInfo().Subject.ToString());

	infoDictionary->WriteKey("Keywords");
	infoDictionary->WriteLiteralStringValue(mTrailerInformation.GetInfo().Keywords.ToString());

	infoDictionary->WriteKey("Creator");
	infoDictionary->WriteLiteralStringValue(mTrailerInformation.GetInfo().Creator.ToString());

	infoDictionary->WriteKey("Producer");
	infoDictionary->WriteLiteralStringValue(mTrailerInformation.GetInfo().Producer.ToString());

	infoDictionary->WriteKey("CreationDate");
	WriteDateState(inStateWriter,mTrailerInformation.GetInfo().CreationDate);

	infoDictionary->WriteKey("ModDate");
	WriteDateState(inStateWriter,mTrailerInformation.GetInfo().ModDate);

	infoDictionary->WriteKey("Trapped");
	infoDictionary->WriteIntegerValue(mTrailerInformation.GetInfo().Trapped);

	MapIterator<StringToPDFTextString> itAdditionalInfo = mTrailerInformation.GetInfo().GetAdditionaEntriesIterator();

	infoDictionary->WriteKey("mAdditionalInfoEntries");
	DictionaryContext* additionalInfoDictionary = inStateWriter->StartDictionary();
	while(itAdditionalInfo.MoveNext())
	{
		additionalInfoDictionary->WriteKey(itAdditionalInfo.GetKey());
		additionalInfoDictionary->WriteLiteralStringValue(itAdditionalInfo.GetValue().ToString());
	}
	inStateWriter->EndDictionary(additionalInfoDictionary);

	inStateWriter->EndDictionary(infoDictionary);
	inStateWriter->EndIndirectObject();

}

void DocumentContext::WriteDateState(ObjectsContext* inStateWriter,const PDFDate& inDate)
{
	DictionaryContext* dateDictionary = inStateWriter->StartDictionary();

	dateDictionary->WriteKey("Type");
	dateDictionary->WriteNameValue("Date");

	dateDictionary->WriteKey("Year");
	dateDictionary->WriteIntegerValue(inDate.Year);

	dateDictionary->WriteKey("Month");
	dateDictionary->WriteIntegerValue(inDate.Month);

	dateDictionary->WriteKey("Day");
	dateDictionary->WriteIntegerValue(inDate.Day);

	dateDictionary->WriteKey("Hour");
	dateDictionary->WriteIntegerValue(inDate.Hour);

	dateDictionary->WriteKey("Minute");
	dateDictionary->WriteIntegerValue(inDate.Minute);

	dateDictionary->WriteKey("Second");
	dateDictionary->WriteIntegerValue(inDate.Second);

	dateDictionary->WriteKey("UTC");
	dateDictionary->WriteIntegerValue(inDate.UTC);

	dateDictionary->WriteKey("HourFromUTC");
	dateDictionary->WriteIntegerValue(inDate.HourFromUTC);

	dateDictionary->WriteKey("MinuteFromUTC");
	dateDictionary->WriteIntegerValue(inDate.MinuteFromUTC);

	inStateWriter->EndDictionary(dateDictionary);
}

void DocumentContext::WriteCatalogInformationState(ObjectsContext* inStateWriter,ObjectIDType inObjectID)
{
	ObjectIDType rootNodeID;
	if(mCatalogInformation.GetCurrentPageTreeNode())
	{
		rootNodeID = inStateWriter->GetInDirectObjectsRegistry().AllocateNewObjectID();
		WritePageTreeState(inStateWriter,rootNodeID,mCatalogInformation.GetPageTreeRoot(mObjectsContext->GetInDirectObjectsRegistry()));
	}


	inStateWriter->StartNewIndirectObject(inObjectID);
	DictionaryContext* catalogInformation = inStateWriter->StartDictionary();

	catalogInformation->WriteKey("Type");
	catalogInformation->WriteNameValue("CatalogInformation");

	if(mCatalogInformation.GetCurrentPageTreeNode())
	{
		catalogInformation->WriteKey("PageTreeRoot");
		catalogInformation->WriteNewObjectReferenceValue(rootNodeID);

		catalogInformation->WriteKey("mCurrentPageTreeNode");
		catalogInformation->WriteNewObjectReferenceValue(mCurrentPageTreeIDInState);
	}

	inStateWriter->EndDictionary(catalogInformation);
	inStateWriter->EndIndirectObject();
	
}

void DocumentContext::WritePageTreeState(ObjectsContext* inStateWriter,ObjectIDType inObjectID,PageTree* inPageTree)
{
	ObjectIDTypeList kidsObjectIDs;

	inStateWriter->StartNewIndirectObject(inObjectID);
	DictionaryContext* pageTreeDictionary = inStateWriter->StartDictionary();
	
	pageTreeDictionary->WriteKey("Type");
	pageTreeDictionary->WriteNameValue("PageTree");

	pageTreeDictionary->WriteKey("mPageTreeID");
	pageTreeDictionary->WriteIntegerValue(inPageTree->GetID());

	pageTreeDictionary->WriteKey("mIsLeafParent");
	pageTreeDictionary->WriteBooleanValue(inPageTree->IsLeafParent());

	if(inPageTree->IsLeafParent())
	{
		pageTreeDictionary->WriteKey("mKidsIDs");
		inStateWriter->StartArray();
		for(int i=0;i<inPageTree->GetNodesCount();++i)
			inStateWriter->WriteInteger(inPageTree->GetPageIDChild(i));
		inStateWriter->EndArray(eTokenSeparatorEndLine);
	}
	else
	{
		pageTreeDictionary->WriteKey("mKidsNodes");
		inStateWriter->StartArray();
		for(int i=0;i<inPageTree->GetNodesCount();++i)
		{
			ObjectIDType pageNodeObjectID = inStateWriter->GetInDirectObjectsRegistry().AllocateNewObjectID();
			inStateWriter->WriteNewIndirectObjectReference(pageNodeObjectID);
			kidsObjectIDs.push_back(pageNodeObjectID);
		}
		inStateWriter->EndArray(eTokenSeparatorEndLine);		
	}

	inStateWriter->EndDictionary(pageTreeDictionary);
	inStateWriter->EndIndirectObject();

	if(kidsObjectIDs.size() > 0)
	{
		ObjectIDTypeList::iterator it = kidsObjectIDs.begin();
		int i = 0;
		for(;i < inPageTree->GetNodesCount();++i,++it)
			WritePageTreeState(inStateWriter,*it,inPageTree->GetPageTreeChild(i));
	}

	if(inPageTree == mCatalogInformation.GetCurrentPageTreeNode())
	{
		mCurrentPageTreeIDInState = inObjectID;
	}
}

EStatusCode DocumentContext::ReadState(PDFParser* inStateReader,ObjectIDType inObjectID)
{
	PDFObjectCastPtr<PDFDictionary> documentState(inStateReader->ParseNewObject(inObjectID));

    PDFObjectCastPtr<PDFBoolean> modifiedDocumentExists(documentState->QueryDirectObject("mModifiedDocumentIDExists"));
    mModifiedDocumentIDExists = modifiedDocumentExists->GetValue();                        
    
    if(mModifiedDocumentIDExists)
    {
        PDFObjectCastPtr<PDFHexString> modifiedDocumentExists(documentState->QueryDirectObject("mModifiedDocumentID"));
        mModifiedDocumentID = modifiedDocumentExists->GetValue();
    }    
    
	PDFObjectCastPtr<PDFDictionary> trailerInformationState(inStateReader->QueryDictionaryObject(documentState.GetPtr(),"mTrailerInformation"));
	ReadTrailerState(inStateReader,trailerInformationState.GetPtr());

	PDFObjectCastPtr<PDFDictionary> catalogInformationState(inStateReader->QueryDictionaryObject(documentState.GetPtr(),"mCatalogInformation"));
	ReadCatalogInformationState(inStateReader,catalogInformationState.GetPtr());

	PDFObjectCastPtr<PDFIndirectObjectReference> usedFontsInformationStateID(documentState->QueryDirectObject("mUsedFontsRepository"));

	return mUsedFontsRepository.ReadState(inStateReader,usedFontsInformationStateID->mObjectID);
}

void DocumentContext::ReadTrailerState(PDFParser* inStateReader,PDFDictionary* inTrailerState)
{
	PDFObjectCastPtr<PDFInteger> prevState(inTrailerState->QueryDirectObject("mPrev"));
	mTrailerInformation.SetPrev(prevState->GetValue());

	PDFObjectCastPtr<PDFDictionary> rootReferenceState(inTrailerState->QueryDirectObject("mRootReference"));
	mTrailerInformation.SetRoot(GetReferenceFromState(rootReferenceState.GetPtr()));

	PDFObjectCastPtr<PDFDictionary> encryptReferenceState(inTrailerState->QueryDirectObject("mEncryptReference"));
	mTrailerInformation.SetEncrypt(GetReferenceFromState(encryptReferenceState.GetPtr()));

	PDFObjectCastPtr<PDFDictionary> infoDictionaryState(inStateReader->QueryDictionaryObject(inTrailerState,"mInfoDictionary"));
	ReadTrailerInfoState(inStateReader,infoDictionaryState.GetPtr());

	PDFObjectCastPtr<PDFDictionary> infoDictionaryReferenceState(inTrailerState->QueryDirectObject("mInfoDictionaryReference"));
	mTrailerInformation.SetInfoDictionaryReference(GetReferenceFromState(infoDictionaryReferenceState.GetPtr()));

}

ObjectReference DocumentContext::GetReferenceFromState(PDFDictionary* inDictionary)
{
    PDFObjectCastPtr<PDFInteger> objectID(inDictionary->QueryDirectObject("ObjectID"));
    PDFObjectCastPtr<PDFInteger> generationNumber(inDictionary->QueryDirectObject("GenerationNumber"));
    
    return ObjectReference((ObjectIDType)(objectID->GetValue()),(unsigned long)generationNumber->GetValue());
}

void DocumentContext::ReadTrailerInfoState(PDFParser* inStateReader,PDFDictionary* inTrailerInfoState)
{
	PDFObjectCastPtr<PDFLiteralString> titleState(inTrailerInfoState->QueryDirectObject("Title"));
	mTrailerInformation.GetInfo().Title = titleState->GetValue();

	PDFObjectCastPtr<PDFLiteralString> authorState(inTrailerInfoState->QueryDirectObject("Author"));
	mTrailerInformation.GetInfo().Author = authorState->GetValue();

	PDFObjectCastPtr<PDFLiteralString> subjectState(inTrailerInfoState->QueryDirectObject("Subject"));
	mTrailerInformation.GetInfo().Subject = subjectState->GetValue();

	PDFObjectCastPtr<PDFLiteralString> keywordsState(inTrailerInfoState->QueryDirectObject("Keywords"));
	mTrailerInformation.GetInfo().Keywords = keywordsState->GetValue();

	PDFObjectCastPtr<PDFLiteralString> creatorState(inTrailerInfoState->QueryDirectObject("Creator"));
	mTrailerInformation.GetInfo().Creator = creatorState->GetValue();

	PDFObjectCastPtr<PDFLiteralString> producerState(inTrailerInfoState->QueryDirectObject("Producer"));
	mTrailerInformation.GetInfo().Producer = producerState->GetValue();

	PDFObjectCastPtr<PDFDictionary> creationDateState(inTrailerInfoState->QueryDirectObject("CreationDate"));
	ReadDateState(creationDateState.GetPtr(),mTrailerInformation.GetInfo().CreationDate);

	PDFObjectCastPtr<PDFDictionary> modDateState(inTrailerInfoState->QueryDirectObject("ModDate"));
	ReadDateState(creationDateState.GetPtr(),mTrailerInformation.GetInfo().ModDate);

	PDFObjectCastPtr<PDFInteger> trappedState(inTrailerInfoState->QueryDirectObject("Trapped"));
	mTrailerInformation.GetInfo().Trapped = (EInfoTrapped)trappedState->GetValue();

	PDFObjectCastPtr<PDFDictionary> additionalInfoState(inTrailerInfoState->QueryDirectObject("mAdditionalInfoEntries"));

	MapIterator<PDFNameToPDFObjectMap> it = additionalInfoState->GetIterator();
	PDFObjectCastPtr<PDFName> keyState;
	PDFObjectCastPtr<PDFLiteralString> valueState;

	mTrailerInformation.GetInfo().ClearAdditionalInfoEntries();
	while(it.MoveNext())
	{
		keyState = it.GetKey();
		valueState = it.GetValue();

		mTrailerInformation.GetInfo().AddAdditionalInfoEntry(keyState->GetValue(),PDFTextString(valueState->GetValue()));
	}
}

void DocumentContext::ReadDateState(PDFDictionary* inDateState,PDFDate& inDate)
{
	PDFObjectCastPtr<PDFInteger> yearState(inDateState->QueryDirectObject("Year"));
	inDate.Year = (int)yearState->GetValue();

	PDFObjectCastPtr<PDFInteger> monthState(inDateState->QueryDirectObject("Month"));
	inDate.Month = (int)monthState->GetValue();

	PDFObjectCastPtr<PDFInteger> dayState(inDateState->QueryDirectObject("Day"));
	inDate.Day = (int)dayState->GetValue();

	PDFObjectCastPtr<PDFInteger> hourState(inDateState->QueryDirectObject("Hour"));
	inDate.Hour = (int)hourState->GetValue();

	PDFObjectCastPtr<PDFInteger> minuteState(inDateState->QueryDirectObject("Minute"));
	inDate.Minute = (int)minuteState->GetValue();

	PDFObjectCastPtr<PDFInteger> secondState(inDateState->QueryDirectObject("Second"));
	inDate.Second = (int)secondState->GetValue();

	PDFObjectCastPtr<PDFInteger> utcState(inDateState->QueryDirectObject("UTC"));
	inDate.UTC = (PDFDate::EUTCRelation)utcState->GetValue();

	PDFObjectCastPtr<PDFInteger> hourFromUTCState(inDateState->QueryDirectObject("HourFromUTC"));
	inDate.HourFromUTC = (int)hourFromUTCState->GetValue();

	PDFObjectCastPtr<PDFInteger> minuteFromUTCState(inDateState->QueryDirectObject("MinuteFromUTC"));
	inDate.MinuteFromUTC = (int)minuteFromUTCState->GetValue();
}

void DocumentContext::ReadCatalogInformationState(PDFParser* inStateReader,PDFDictionary* inCatalogInformationState)
{
	PDFObjectCastPtr<PDFIndirectObjectReference> pageTreeRootState(inCatalogInformationState->QueryDirectObject("PageTreeRoot"));

	// clear current state
	if(mCatalogInformation.GetCurrentPageTreeNode())
	{
		delete mCatalogInformation.GetPageTreeRoot(mObjectsContext->GetInDirectObjectsRegistry());
		mCatalogInformation.SetCurrentPageTreeNode(NULL);
	}


	if(!pageTreeRootState) // no page nodes yet...
		return;

	PDFObjectCastPtr<PDFIndirectObjectReference> currentPageTreeState(inCatalogInformationState->QueryDirectObject("mCurrentPageTreeNode"));
	mCurrentPageTreeIDInState = currentPageTreeState->mObjectID;

	PDFObjectCastPtr<PDFDictionary> pageTreeState(inStateReader->ParseNewObject(pageTreeRootState->mObjectID));
	
	PDFObjectCastPtr<PDFInteger> pageTreeIDState(pageTreeState->QueryDirectObject("mPageTreeID"));
	PageTree* rootNode = new PageTree((ObjectIDType)pageTreeIDState->GetValue());

	if(pageTreeRootState->mObjectID == mCurrentPageTreeIDInState)
		mCatalogInformation.SetCurrentPageTreeNode(rootNode);
	ReadPageTreeState(inStateReader,pageTreeState.GetPtr(),rootNode);

}

void DocumentContext::ReadPageTreeState(PDFParser* inStateReader,PDFDictionary* inPageTreeState,PageTree* inPageTree)
{
	PDFObjectCastPtr<PDFBoolean> isLeafParentState(inPageTreeState->QueryDirectObject("mIsLeafParent"));
	bool isLeafParent = isLeafParentState->GetValue();
		
	if(isLeafParent)
	{
		PDFObjectCastPtr<PDFArray> kidsIDsState(inPageTreeState->QueryDirectObject("mKidsIDs"));
		PDFObjectCastPtr<PDFInteger> kidID;
		
		SingleValueContainerIterator<PDFObjectVector> it = kidsIDsState->GetIterator();
		while(it.MoveNext())
		{
			kidID = it.GetItem();
			inPageTree->AddNodeToTree((ObjectIDType)kidID->GetValue(),mObjectsContext->GetInDirectObjectsRegistry());
		}
	}
	else
	{
		PDFObjectCastPtr<PDFArray> kidsNodesState(inPageTreeState->QueryDirectObject("mKidsNodes"));

		SingleValueContainerIterator<PDFObjectVector> it = kidsNodesState->GetIterator();
		while(it.MoveNext())
		{
			PDFObjectCastPtr<PDFDictionary> kidNodeState(inStateReader->ParseNewObject(((PDFIndirectObjectReference*)it.GetItem())->mObjectID));

			PDFObjectCastPtr<PDFInteger> pageTreeIDState(kidNodeState->QueryDirectObject("mPageTreeID"));
			PageTree* kidNode = new PageTree((ObjectIDType)pageTreeIDState->GetValue());

			if(((PDFIndirectObjectReference*)it.GetItem())->mObjectID == mCurrentPageTreeIDInState)
				mCatalogInformation.SetCurrentPageTreeNode(kidNode);
			ReadPageTreeState(inStateReader,kidNodeState.GetPtr(),kidNode);

			inPageTree->AddNodeToTree(kidNode,mObjectsContext->GetInDirectObjectsRegistry());
		}
	}
}

PDFDocumentCopyingContext* DocumentContext::CreatePDFCopyingContext(const string& inFilePath)
{
	PDFDocumentCopyingContext* context = new PDFDocumentCopyingContext();

	if(context->Start(inFilePath,this,mObjectsContext,mParserExtender) != PDFHummus::eSuccess)
	{
		delete context;
		return NULL;
	}
	else
		return context;
}

EStatusCode DocumentContext::AttachURLLinktoCurrentPage(const string& inURL,const PDFRectangle& inLinkClickArea)
{
	EStatusCodeAndObjectIDType writeResult = WriteAnnotationAndLinkForURL(inURL,inLinkClickArea);

	if(writeResult.first != PDFHummus::eSuccess)
		return writeResult.first;

	RegisterAnnotationReferenceForNextPageWrite(writeResult.second);
	return PDFHummus::eSuccess;
}

static const string scAnnot = "Annot";
static const string scLink = "Link";
static const string scRect = "Rect";
static const string scF = "F";
static const string scW = "W";
static const string scA = "A";
static const string scBS = "BS";
static const string scAction = "Action";
static const string scS = "S";
static const string scURI = "URI";
EStatusCodeAndObjectIDType DocumentContext::WriteAnnotationAndLinkForURL(const string& inURL,const PDFRectangle& inLinkClickArea)
{
	EStatusCodeAndObjectIDType result(PDFHummus::eFailure,0);

	do
	{
		Ascii7Encoding encoding;

		BoolAndString encodedResult = encoding.Encode(inURL);
		if(!encodedResult.first)
		{
			TRACE_LOG1("DocumentContext::WriteAnnotationAndLinkForURL, unable to encode string to Ascii7. make sure that all charachters are valid URLs [should be ascii 7 compatible]. URL - %s",inURL.c_str());
			break;
		}

		result.second = mObjectsContext->StartNewIndirectObject();
		DictionaryContext* linkAnnotationContext = mObjectsContext->StartDictionary();

		// Type
		linkAnnotationContext->WriteKey(scType);
		linkAnnotationContext->WriteNameValue(scAnnot);

		// Subtype
		linkAnnotationContext->WriteKey(scSubType);
		linkAnnotationContext->WriteNameValue(scLink);

		// Rect
		linkAnnotationContext->WriteKey(scRect);
		linkAnnotationContext->WriteRectangleValue(inLinkClickArea);

		// F
		linkAnnotationContext->WriteKey(scF);
		linkAnnotationContext->WriteIntegerValue(4);

		// BS
		linkAnnotationContext->WriteKey(scBS);
		DictionaryContext* borderStyleContext = mObjectsContext->StartDictionary();

		borderStyleContext->WriteKey(scW);
		borderStyleContext->WriteIntegerValue(0);
		mObjectsContext->EndDictionary(borderStyleContext);

		// A
		linkAnnotationContext->WriteKey(scA);
		DictionaryContext* actionContext = mObjectsContext->StartDictionary();

		// Type
		actionContext->WriteKey(scType);
		actionContext->WriteNameValue(scAction);

		// S
		actionContext->WriteKey(scS);
		actionContext->WriteNameValue(scURI);

		// URI
		actionContext->WriteKey(scURI);
		actionContext->WriteLiteralStringValue(encodedResult.second);
		
		mObjectsContext->EndDictionary(actionContext);

		mObjectsContext->EndDictionary(linkAnnotationContext);
		mObjectsContext->EndIndirectObject();
		result.first = PDFHummus::eSuccess;
	}while(false);

	return result;
}

void DocumentContext::RegisterAnnotationReferenceForNextPageWrite(ObjectIDType inAnnotationReference)
{
	mAnnotations.insert(inAnnotationReference);
}

EStatusCode DocumentContext::MergePDFPagesToPage(PDFPage* inPage,
								const string& inPDFFilePath,
								const PDFPageRange& inPageRange,
								const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.MergePDFPagesToPage(inPage,
												   inPDFFilePath,
												   inPageRange,
												   inCopyAdditionalObjects);
}

PDFImageXObject* DocumentContext::CreateImageXObjectFromJPGStream(IByteReaderWithPosition* inJPGStream)
{
	return mJPEGImageHandler.CreateImageXObjectFromJPGStream(inJPGStream);
}

PDFImageXObject* DocumentContext::CreateImageXObjectFromJPGStream(IByteReaderWithPosition* inJPGStream,ObjectIDType inImageXObjectID)
{
	return mJPEGImageHandler.CreateImageXObjectFromJPGStream(inJPGStream,inImageXObjectID);
}

PDFFormXObject* DocumentContext::CreateFormXObjectFromJPGStream(IByteReaderWithPosition* inJPGStream)
{
	return mJPEGImageHandler.CreateFormXObjectFromJPGStream(inJPGStream);

}

PDFFormXObject* DocumentContext::CreateFormXObjectFromJPGStream(IByteReaderWithPosition* inJPGStream,ObjectIDType inFormXObjectID)
{
	return mJPEGImageHandler.CreateFormXObjectFromJPGStream(inJPGStream,inFormXObjectID);
}

PDFFormXObject* DocumentContext::CreateFormXObjectFromTIFFStream(IByteReaderWithPosition* inTIFFStream,
															const TIFFUsageParameters& inTIFFUsageParameters)
{
	return mTIFFImageHandler.CreateFormXObjectFromTIFFStream(inTIFFStream,inTIFFUsageParameters);
}

PDFFormXObject* DocumentContext::CreateFormXObjectFromTIFFStream(IByteReaderWithPosition* inTIFFStream,
															ObjectIDType inFormXObjectID,
															const TIFFUsageParameters& inTIFFUsageParameters)
{
	return mTIFFImageHandler.CreateFormXObjectFromTIFFStream(inTIFFStream,inFormXObjectID,inTIFFUsageParameters);
}

EStatusCodeAndObjectIDTypeList DocumentContext::CreateFormXObjectsFromPDF(IByteReaderWithPosition* inPDFStream,
																	const PDFPageRange& inPageRange,
																	EPDFPageBox inPageBoxToUseAsFormBox,
																	const double* inTransformationMatrix,
																	const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.CreateFormXObjectsFromPDF(inPDFStream,inPageRange,inPageBoxToUseAsFormBox,inTransformationMatrix,inCopyAdditionalObjects);
}

EStatusCodeAndObjectIDTypeList DocumentContext::CreateFormXObjectsFromPDF(IByteReaderWithPosition* inPDFStream,
																	const PDFPageRange& inPageRange,
																	const PDFRectangle& inCropBox,
																	const double* inTransformationMatrix,
																	const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.CreateFormXObjectsFromPDF(inPDFStream,inPageRange,inCropBox,inTransformationMatrix,inCopyAdditionalObjects);
}

EStatusCodeAndObjectIDTypeList DocumentContext::AppendPDFPagesFromPDF(IByteReaderWithPosition* inPDFStream,
																const PDFPageRange& inPageRange,
																const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.AppendPDFPagesFromPDF(inPDFStream,inPageRange,inCopyAdditionalObjects);
}

EStatusCode DocumentContext::MergePDFPagesToPage(	PDFPage* inPage,
											IByteReaderWithPosition* inPDFStream,
											const PDFPageRange& inPageRange,
											const ObjectIDTypeList& inCopyAdditionalObjects)
{
	return mPDFDocumentHandler.MergePDFPagesToPage(inPage,inPDFStream,inPageRange,inCopyAdditionalObjects);
}

PDFDocumentCopyingContext* DocumentContext::CreatePDFCopyingContext(IByteReaderWithPosition* inPDFStream)
{
	PDFDocumentCopyingContext* context = new PDFDocumentCopyingContext();

	if(context->Start(inPDFStream,this,mObjectsContext,mParserExtender) != PDFHummus::eSuccess)
	{
		delete context;
		return NULL;
	}
	else
		return context;
}

void DocumentContext::Cleanup()
{
	// DO NOT NULL MOBJECTSCONTEXT. EVER

	mTrailerInformation.Reset();
	mCatalogInformation.Reset();
	mJPEGImageHandler.Reset();
	mTIFFImageHandler.Reset();
	mUsedFontsRepository.Reset();
	mOutputFilePath.clear();
	mExtenders.clear();
	mAnnotations.clear();
	mCopyingContexts.clear();
    mModifiedDocumentIDExists = false;
    
    ResourcesDictionaryAndStringToIResourceWritingTaskListMap::iterator itCategories = mResourcesTasks.begin();
    
    for(; itCategories != mResourcesTasks.end(); ++itCategories)
    {
        IResourceWritingTaskList::iterator itWritingTasks = itCategories->second.begin();
        for(; itWritingTasks != itCategories->second.end(); ++itWritingTasks)
            delete *itWritingTasks;
    }
    
    mResourcesTasks.clear();
    
    PDFFormXObjectToIFormEndWritingTaskListMap::iterator itFormEnd = mFormEndTasks.begin();
    
    for(; itFormEnd != mFormEndTasks.end();++itFormEnd)
    {
        IFormEndWritingTaskList::iterator itEndWritingTasks = itFormEnd->second.begin();
        for(; itEndWritingTasks != itFormEnd->second.end(); ++itEndWritingTasks)
            delete *itEndWritingTasks;
        
    }
    mFormEndTasks.clear();
}

void DocumentContext::SetParserExtender(IPDFParserExtender* inParserExtender)
{
	mParserExtender = inParserExtender;
	mPDFDocumentHandler.SetParserExtender(inParserExtender);
}

void DocumentContext::RegisterCopyingContext(PDFDocumentCopyingContext* inCopyingContext)
{
	mCopyingContexts.insert(inCopyingContext);
}

void DocumentContext::UnRegisterCopyingContext(PDFDocumentCopyingContext* inCopyingContext)
{
	mCopyingContexts.erase(inCopyingContext);
}

EStatusCode DocumentContext::SetupModifiedFile(PDFParser* inModifiedFileParser)
{
    // setup trailer and save original document ID
    
    if(!inModifiedFileParser->GetTrailer())
        return eFailure;
    
    PDFObjectCastPtr<PDFIndirectObjectReference> rootReference = inModifiedFileParser->GetTrailer()->QueryDirectObject("Root");
    if(!rootReference)
        return eFailure;
    
    // set catalog reference and previous reference table position
    mTrailerInformation.SetRoot(rootReference->mObjectID);
    mTrailerInformation.SetPrev(inModifiedFileParser->GetXrefPosition());
    
    // setup modified date to current time
    mTrailerInformation.GetInfo().ModDate.SetToCurrentTime();
    
    // try to get document ID. in any case use whatever was the original
    mModifiedDocumentIDExists = true;
    mModifiedDocumentID = "";
    PDFObjectCastPtr<PDFArray> idArray = inModifiedFileParser->GetTrailer()->QueryDirectObject("ID");
    if(idArray.GetPtr() && idArray->GetLength() == 2)
    {
        PDFObjectCastPtr<PDFHexString> firstID = idArray->QueryObject(0);
        if(firstID.GetPtr())
            mModifiedDocumentID = firstID->GetValue();
    }
    
    return eSuccess;
}

class VersionUpdate : public DocumentContextExtenderAdapter
{
public:
    VersionUpdate(EPDFVersion inPDFVersion){mPDFVersion = inPDFVersion;}
    virtual ~VersionUpdate(){}
    
    // IDocumentContextExtender implementation
	virtual PDFHummus::EStatusCode OnCatalogWrite(
            CatalogInformation* inCatalogInformation,
            DictionaryContext* inCatalogDictionaryContext,
            ObjectsContext* inPDFWriterObjectContext,
            PDFHummus::DocumentContext* inDocumentContext)
    {
        inCatalogDictionaryContext->WriteKey("Version");
        
        // need to write as /1.4 (name, of float value)
        inCatalogDictionaryContext->WriteNameValue(Double(((double)mPDFVersion)/10).ToString());
        
        return eSuccess;
    }    
    
private:
    
    EPDFVersion mPDFVersion;
};

EStatusCode	DocumentContext::FinalizeModifiedPDF(PDFParser* inModifiedFileParser,EPDFVersion inModifiedPDFVersion)
{
	EStatusCode status;
	LongFilePositionType xrefTablePosition;
    
	do
	{
		status = WriteUsedFontsDefinitions();
		if(status != eSuccess)
			break;
        
        // Page tree writing
        // k. page tree needs to be a combination of what pages are coming from the original document
        // and those from the new one. The decision whether a new page tree need to be written is simple -
        // if no pages were added...no new page tree...if yes...then we need a new page tree which will combine
        // the new pages and the old pages
        
        ObjectReference originalDocumentPageTreeRoot = GetOriginalDocumentPageTreeRoot(inModifiedFileParser);
        bool hasNewPageTreeRoot;
        ObjectReference finalPageRoot;
        
        if(DocumentHasNewPages())
        {
            if(originalDocumentPageTreeRoot.ObjectID != 0)
            {
                finalPageRoot.ObjectID = WriteCombinedPageTree(inModifiedFileParser);
                finalPageRoot.GenerationNumber = 0;
                
                // check for error - may fail to write combined page tree if document is protected!
                if(finalPageRoot.ObjectID == 0)
                {
                    status = eFailure;
                    break;
                }
            }
            else
            {
                WritePagesTree();
                PageTree* pageTreeRoot = mCatalogInformation.GetPageTreeRoot(mObjectsContext->GetInDirectObjectsRegistry());
                finalPageRoot.ObjectID = pageTreeRoot->GetID();
                finalPageRoot.GenerationNumber = 0;

            }
            hasNewPageTreeRoot = true;
        }
        else
        {
            hasNewPageTreeRoot = false;
            finalPageRoot = originalDocumentPageTreeRoot;
        }
        // marking if has new page root, cause this effects the decision to have a new catalog
        
        bool requiresVersionUpdate = IsRequiredVersionHigherThanPDFVersion(inModifiedFileParser,inModifiedPDFVersion);
        
        if(hasNewPageTreeRoot || requiresVersionUpdate || DoExtendersRequireCatalogUpdate(inModifiedFileParser))
        {
            VersionUpdate* versionUpdate = NULL;
            if(requiresVersionUpdate)
            {
                versionUpdate = new VersionUpdate(inModifiedPDFVersion);
                AddDocumentContextExtender(versionUpdate);
            }
            status = WriteCatalogObject(finalPageRoot);
            if(requiresVersionUpdate)
            {
                RemoveDocumentContextExtender(versionUpdate);
                delete versionUpdate;
            }
            if(status != 0)
                break;
        }
                
 		// write the info dictionary of the trailer, if has any valid entries
		WriteInfoDictionary();
        
        if(RequiresXrefStream(inModifiedFileParser))
        {
            status = WriteXrefStream(xrefTablePosition);
        }
        else
        {
            status = mObjectsContext->WriteXrefTable(xrefTablePosition);
            if(status != eSuccess)
                break;
            
            status = WriteTrailerDictionary();
            if(status != eSuccess)
                break;
            
        }
        
		WriteXrefReference(xrefTablePosition);
		WriteFinalEOF();
	} while(false);
    
	return status;
}

ObjectReference DocumentContext::GetOriginalDocumentPageTreeRoot(PDFParser* inModifiedFileParser)
{
	ObjectReference rootObject;
    
	
	do
	{
		// get catalogue, verify indirect reference
		PDFObjectCastPtr<PDFIndirectObjectReference> catalogReference(inModifiedFileParser->GetTrailer()->QueryDirectObject("Root"));
		if(!catalogReference)
		{
			TRACE_LOG("DocumentContext::GetOriginalDocumentPageTreeRoot, failed to read catalog reference in trailer");
			break;
		}
        
		PDFObjectCastPtr<PDFDictionary> catalog(inModifiedFileParser->ParseNewObject(catalogReference->mObjectID));
		if(!catalog)
		{
			TRACE_LOG("DocumentContext::GetOriginalDocumentPageTreeRoot, failed to read catalog");
			break;
		}
        
		// get pages, verify indirect reference
		PDFObjectCastPtr<PDFIndirectObjectReference> pagesReference(catalog->QueryDirectObject("Pages"));
		if(!pagesReference)
		{
			TRACE_LOG("PDFParser::GetOriginalDocumentPageTreeRoot, failed to read pages reference in catalog");
			break;
		}
        
        rootObject.GenerationNumber = pagesReference->mVersion;
        rootObject.ObjectID = pagesReference->mObjectID;
                
	}while(false);
    
	return rootObject;    
    
}

bool DocumentContext::DocumentHasNewPages()
{
    // the best way to check if there are new pages created is to check if there's at least one leaf
    
    if(!mCatalogInformation.GetCurrentPageTreeNode())
        return false;
    
    // note that page tree root surely exist, so no worries about creating a new one
    PageTree* pageTreeRoot = mCatalogInformation.GetPageTreeRoot(mObjectsContext->GetInDirectObjectsRegistry());
    
    bool hasLeafs = false;
    
    while(hasLeafs == false)
    {
        hasLeafs = pageTreeRoot->IsLeafParent();
        if(pageTreeRoot->GetNodesCount() == 0)
            break;
        else 
            pageTreeRoot = pageTreeRoot->GetPageTreeChild(0);
    }
 
    return hasLeafs;
}

ObjectIDType DocumentContext::WriteCombinedPageTree(PDFParser* inModifiedFileParser)
{
    // writing a combined page tree looks like this
    // first, we allocate a new root object that will contain both new and old pages
    // then, write the new pages tree with reference to the new root object as parent
    // then, write a new pages tree root to represent the old pages tree. this is a copy
    // of the old tree, but with the parent object pointing to the new root object.
    // now write the new root object with allocated ID and the old and new pages trees roots as direct children.
    // happy.
    
    
    // allocate new root object
    ObjectIDType newPageRootTreeID = mObjectsContext->GetInDirectObjectsRegistry().AllocateNewObjectID();
    
    PageTree* root = new PageTree(newPageRootTreeID);
	
    // write new pages tree
    PageTree* newPagesTree = mCatalogInformation.GetPageTreeRoot(mObjectsContext->GetInDirectObjectsRegistry());
    newPagesTree->SetParent(root);
 	long long newPagesCount = WritePageTree(newPagesTree);
    newPagesTree->SetParent(NULL);
    delete root;
    
    // write modified old pages root
    ObjectReference originalTreeRoot = GetOriginalDocumentPageTreeRoot(inModifiedFileParser);
    
    PDFObjectCastPtr<PDFDictionary> originalTreeRootObject = inModifiedFileParser->ParseNewObject(originalTreeRoot.ObjectID);
  
    mObjectsContext->StartModifiedIndirectObject(originalTreeRoot.ObjectID);
    
    DictionaryContext* pagesTreeContext = mObjectsContext->StartDictionary();
 
    PDFObjectCastPtr<PDFInteger> kidsCount = originalTreeRootObject->QueryDirectObject(scCount);
    long long originalPageTreeKidsCount = kidsCount.GetPtr() ? kidsCount->GetValue() : 0;
    
    // copy all but parent key. then add parent as the new root object
    
    MapIterator<PDFNameToPDFObjectMap>  pageTreeIt = originalTreeRootObject->GetIterator();
    PDFDocumentCopyingContext aCopyingContext;
    
    EStatusCode status = aCopyingContext.Start(inModifiedFileParser,this,mObjectsContext,NULL);
    
    do {
        
        if(status != eSuccess)
        {
            TRACE_LOG("DocumentContext::WriteCombinedPageTree, Unable to copy original page tree. this probably means that the original file is protected - and is therefore unsupported for such activity as adding pages");
            break;
        }
        
        while(pageTreeIt.MoveNext())
        {
            if(pageTreeIt.GetKey()->GetValue() != "Parent")
            {
                pagesTreeContext->WriteKey(pageTreeIt.GetKey()->GetValue());
                aCopyingContext.CopyDirectObjectAsIs(pageTreeIt.GetValue());
            }
            
        }
        
        aCopyingContext.End();
    
        // parent
        pagesTreeContext->WriteKey(scParent);
        pagesTreeContext->WriteNewObjectReferenceValue(newPageRootTreeID);
        
        mObjectsContext->EndDictionary(pagesTreeContext);
        mObjectsContext->EndIndirectObject();
        
        // now write the root page tree. 2 kids, the original pages, and new pages
        mObjectsContext->StartNewIndirectObject(newPageRootTreeID);
        
        pagesTreeContext = mObjectsContext->StartDictionary();
        
        // type
        pagesTreeContext->WriteKey(scType);
        pagesTreeContext->WriteNameValue(scPages);
        
        // count
        pagesTreeContext->WriteKey(scCount);    
        pagesTreeContext->WriteIntegerValue(originalPageTreeKidsCount + newPagesCount);
        
        // kids
        pagesTreeContext->WriteKey(scKids);
        mObjectsContext->StartArray();
        
        mObjectsContext->WriteIndirectObjectReference(originalTreeRoot);
        mObjectsContext->WriteNewIndirectObjectReference(newPagesTree->GetID());
        
        mObjectsContext->EndArray();
        mObjectsContext->EndLine();

        mObjectsContext->EndDictionary(pagesTreeContext);
        mObjectsContext->EndIndirectObject();
    
    } while (false);
   
    

    if(status == eSuccess)
        return newPageRootTreeID;
    else
        return 0;
}
           
bool DocumentContext::IsRequiredVersionHigherThanPDFVersion(PDFParser* inModifiedFileParser,EPDFVersion inModifiedPDFVersion)
{
    return (EPDFVersion)((size_t)(inModifiedFileParser->GetPDFLevel() * 10)) < inModifiedPDFVersion;
}

bool DocumentContext::DoExtendersRequireCatalogUpdate(PDFParser* inModifiedFileParser)
{
    bool isUpdateRequired = false;
    
 	IDocumentContextExtenderSet::iterator it = mExtenders.begin();
	for(; it != mExtenders.end() && !isUpdateRequired; ++it)
		isUpdateRequired = (*it)->IsCatalogUpdateRequiredForModifiedFile(inModifiedFileParser);
    
    return isUpdateRequired;
}

bool DocumentContext::RequiresXrefStream(PDFParser* inModifiedFileParser)
{
    // modification requires xref stream if the original document uses one...so just ask trailer
    if(!inModifiedFileParser->GetTrailer())
        return false;
    
    PDFObjectCastPtr<PDFName> typeObject = inModifiedFileParser->GetTrailer()->QueryDirectObject("Type");
    
    if(!typeObject)
        return false;
    
    return typeObject->GetValue() == "XRef";
    
    
}

EStatusCode DocumentContext::WriteXrefStream(LongFilePositionType& outXrefPosition)
{
    EStatusCode status = eSuccess;
    
    do 
    {
        // get the position by accessing the free context of the underlying objects stream
 
        // an Xref stream is a beast that is both trailer and the xref
        // start the xref with a dictionary detailing the trailer information, then move to the
        // xref table aspects, with the lower level objects context.
        
        outXrefPosition = mObjectsContext->StartFreeContext()->GetCurrentPosition();
        mObjectsContext->StartNewIndirectObject();
 
        mObjectsContext->EndFreeContext();
       
        DictionaryContext* xrefDictionary = mObjectsContext->StartDictionary();
        
        xrefDictionary->WriteKey("Type");
        xrefDictionary->WriteNameValue("XRef");
        
        status = WriteTrailerDictionaryValues(xrefDictionary);
        if(status != eSuccess)
            break;

        // k. now for the xref table itself
        status = mObjectsContext->WriteXrefStream(xrefDictionary);
        
    } 
    while (false);
    
    return status;
}

PDFDocumentCopyingContext* DocumentContext::CreatePDFCopyingContext(PDFParser* inPDFParser)
{
	PDFDocumentCopyingContext* context = new PDFDocumentCopyingContext();
    
	if(context->Start(inPDFParser,this,mObjectsContext,mParserExtender) != PDFHummus::eSuccess)
	{
		delete context;
		return NULL;
	}
	else
		return context;
}


string DocumentContext::AddExtendedResourceMapping(PDFPage* inPage,
                                  const string& inResourceCategoryName,
                                  IResourceWritingTask* inWritingTask)
{
    return AddExtendedResourceMapping(&inPage->GetResourcesDictionary(),inResourceCategoryName,inWritingTask);
}

string DocumentContext::AddExtendedResourceMapping(ResourcesDictionary* inResourceDictionary,
                                  const string& inResourceCategoryName,
                                  IResourceWritingTask* inWritingTask)
{
    // do two things. first is to include this writing task as part of the tasks to write
    // second is to allocate a name for this resource from the resource category in the relevant dictionary
    
    ResourcesDictionaryAndStringToIResourceWritingTaskListMap::iterator it =
    mResourcesTasks.find(ResourcesDictionaryAndString(inResourceDictionary,inResourceCategoryName));
    
    if(it == mResourcesTasks.end())
    {
        it =mResourcesTasks.insert(
                                       ResourcesDictionaryAndStringToIResourceWritingTaskListMap::value_type(
                                                                                                             ResourcesDictionaryAndString(inResourceDictionary,inResourceCategoryName),
                                                                                                             IResourceWritingTaskList())).first;
    }
    
    it->second.push_back(inWritingTask);
    
    string newResourceName;
    
    if(inResourceCategoryName == scXObjects)
        newResourceName = inResourceDictionary->AddXObjectMapping(0);
    else if(inResourceCategoryName == scExtGStates)
        newResourceName = inResourceDictionary->AddExtGStateMapping(0);
    else if(inResourceCategoryName == scFonts)
        newResourceName = inResourceDictionary->AddFontMapping(0);
    else if(inResourceCategoryName == scColorSpaces)
        newResourceName = inResourceDictionary->AddColorSpaceMapping(0);
    else if(inResourceCategoryName == scPatterns)
        newResourceName = inResourceDictionary->AddPatternMapping(0);
    else if(inResourceCategoryName == scShadings)
        newResourceName = inResourceDictionary->AddShadingMapping(0);
    else if(inResourceCategoryName == scProperties)
        newResourceName = inResourceDictionary->AddPropertyMapping(0);
    else {
        TRACE_LOG1("DocumentContext::AddExtendedResourceMapping:, unidentified category for registering a resource writer %s",inResourceCategoryName.c_str());
    }
    return newResourceName;
}

string DocumentContext::AddExtendedResourceMapping(PDFFormXObject* inFormXObject,
                                                   const string& inResourceCategoryName,
                                                   IResourceWritingTask* inWritingTask)
{
    return AddExtendedResourceMapping(&inFormXObject->GetResourcesDictionary(),inResourceCategoryName,inWritingTask);
}

void DocumentContext::RegisterFormEndWritingTask(PDFFormXObject* inFormXObject,IFormEndWritingTask* inWritingTask)
{
    PDFFormXObjectToIFormEndWritingTaskListMap::iterator it = 
    mFormEndTasks.find(inFormXObject);
    
    if(it == mFormEndTasks.end())
    {
        it =mFormEndTasks.insert(PDFFormXObjectToIFormEndWritingTaskListMap::value_type(inFormXObject,IFormEndWritingTaskList())).first;
    }
    
    it->second.push_back(inWritingTask);    
}

