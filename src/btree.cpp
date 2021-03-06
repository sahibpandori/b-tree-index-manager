/**
 * Project team:
 *  Sahib Singh Pandori  - 9071742556 | pandori@wisc.edu
 *  Haylee Jane Monteiro - 9071694690 | monteiro@wisc.edu
 *  Sreejita Dutta       - 9075301680 | sdutta7@wisc.edu
 *
 * This file contains the implementation of the methods for b+tree index interface as defined in btree.h
 *
 * @author See Contributors.txt for code contributors and overview of BadgerDB.
 *
 * @section LICENSE
 * Copyright (c) 2012 Database Group, Computer Sciences Department, University of Wisconsin-Madison.
 */

#include <climits>
#include <stack>
#include <climits>
#include "btree.h"
#include "filescan.h"
#include "exceptions/bad_index_info_exception.h"
#include "exceptions/bad_opcodes_exception.h"
#include "exceptions/bad_scanrange_exception.h"
#include "exceptions/no_such_key_found_exception.h"
#include "exceptions/scan_not_initialized_exception.h"
#include "exceptions/index_scan_completed_exception.h"
#include "exceptions/file_not_found_exception.h"
#include "exceptions/end_of_file_exception.h"
#include "exceptions/file_exists_exception.h"
#include "exceptions/page_not_pinned_exception.h"
#include "exceptions/hash_not_found_exception.h"
#include "exceptions/page_pinned_exception.h"


//#define DEBUG

namespace badgerdb
{

    // -----------------------------------------------------------------------------
    // BTreeIndex::BTreeIndex -- Constructor
    // -----------------------------------------------------------------------------
    BTreeIndex::BTreeIndex(
            const std::string & relationName,
            std::string & outIndexName,
            BufMgr *bufMgrIn,
            const int attrByteOffset,
            const Datatype attrType) {

        // Create index file name
        std::ostringstream idxStr;
        idxStr << relationName << '.' << attrByteOffset;
        outIndexName = idxStr.str();

        // initialize btree index variables
        bufMgr = bufMgrIn;
        attributeType = attrType;
        this->attrByteOffset = attrByteOffset;
        leafOccupancy = 0;
        nodeOccupancy = 0;
        scanExecuting = false;

        IndexMetaInfo* metadata;
        Page* headerPage;
        Page* rootPage;

        try {
            // Create file, check if it exists
            file = new BlobFile(outIndexName, true);
            // File does not exist, so new index file has been created

            // Allocate index meta info page and btree root page
            bufMgr->allocPage(file, headerPageNum, headerPage);
            bufMgr->allocPage(file, rootPageNum, rootPage);

            // Set up index meta info
            metadata = (IndexMetaInfo*) headerPage;
            strcpy(metadata->relationName, relationName.c_str());
            metadata->attrByteOffset = attrByteOffset;
            metadata->attrType = attrType;
            metadata->rootPageNo = rootPageNum;

            // Set up the root of the btree
            auto root = (NonLeafNodeInt*) rootPage;
            root->level = 1;
            for (int i = 0; i < INTARRAYNONLEAFSIZE; i++) {
                clearNonLeafNodeAtIdx(root, i);
            }
            root->pageNoArray[INTARRAYNONLEAFSIZE] = Page::INVALID_NUMBER;

            // Scan relation and insert entries for all tuples into index
            try {
                FileScan fileScan(relationName, bufMgr);
                RecordId rid = {};
                while (true) {
                    fileScan.scanNext(rid);
                    insertEntry((int*) fileScan.getRecord().c_str() + attrByteOffset, rid);
                }
            } catch (EndOfFileException& e) {
                // Do nothing. Finished scanning file.
            }

            // Unpin header page and root page as they are no longer in use
            try {
                bufMgr->unPinPage(file, headerPageNum, true);
            } catch (PageNotPinnedException& e) {
                // Do nothing.
            }
            try {
                bufMgr->unPinPage(file, rootPageNum, true);
            } catch (PageNotPinnedException& e) {
                // Do nothing.
            }
        } catch (FileExistsException& e) {  // File exists
            // Open the file
            file = new BlobFile(outIndexName, false);

            // Get the meta page number fom the file
            headerPageNum = file->getFirstPageNo();

            // Get index meta info for value checking
            bufMgr->readPage(file, headerPageNum, headerPage);
            metadata = (IndexMetaInfo*) headerPage;

            // Check that values in (relationName, attribute byte, attribute type etc.) match parameters
            if (strcmp(metadata->relationName, relationName.c_str()) != 0
                || metadata->attrByteOffset != attrByteOffset
                || metadata->attrType != attrType) {
                // Metadata does not match the parameters
                // Unpin header page before exiting
                try {
                    bufMgr->unPinPage(file, headerPageNum, false);
                } catch (PageNotPinnedException& e) {
                    // Do nothing.
                }
                throw BadIndexInfoException("Error: Existing index metadata does not match parameters passed.");
            }
            // Metatdata matches

            // Set root page for the index
            rootPageNum = metadata->rootPageNo;

            // Unpin header page
            try {
                bufMgr->unPinPage(file, headerPageNum, false);
            } catch (PageNotPinnedException& e) {
                // Do nothing.
            }
        }
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::~BTreeIndex -- destructor
    // -----------------------------------------------------------------------------
    BTreeIndex::~BTreeIndex() {
        // Clean up state variables
        scanExecuting = false;

        // Unpin any pinned pages
        try {
            bufMgr->unPinPage(file, currentPageNum, false);
        } catch (PageNotPinnedException& e) {
            // Do nothing.
        }

        // Flush index file
        bufMgr->flushFile(file);

        // Delete the index file (calls destructor of File)
        delete file;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::insertEntry
    // -----------------------------------------------------------------------------
    void BTreeIndex::insertEntry(const void *key, const RecordId rid) {
        if (key == nullptr)
            return;

        // Get the root node
        Page *currPage;
        bufMgr->readPage(file, rootPageNum, currPage);
        auto currNode = (NonLeafNodeInt*) currPage;

        LeafNodeInt* dataNode;
        int idx, intKey = *((int*) key);

        // Stack to keep track of all parent nodes in the path to the dataNode
        std::stack<PageId> path;
        path.push(rootPageNum);

        // Traverse the b-tree to find the data node for insertion
        while (true) {

            // Traverse the current level of the tree to get the next page index
            for (idx = 0;
                 idx < INTARRAYNONLEAFSIZE &&
                 currNode->pageNoArray[idx+1] != Page::INVALID_NUMBER &&
                 currNode->keyArray[idx] < intKey;
                 idx++);

            // The node is a newly created b-tree root node
            if (idx == 0 && currNode->pageNoArray[0] == Page::INVALID_NUMBER) {

                // Allocate a page for the new data node
                Page *pageRight, *pageLeft;
                PageId pageIdLeft, pageIdRight;
                bufMgr->allocPage(file, pageIdLeft, pageLeft);
                bufMgr->allocPage(file, pageIdRight, pageRight);

                // Point the root to the data node
                currNode->keyArray[0] = intKey;
                currNode->pageNoArray[0] = pageIdLeft;
                currNode->pageNoArray[1] = pageIdRight;

                // Initialize the data node
                dataNode = (LeafNodeInt*) pageRight;
                auto leftDataNode = (LeafNodeInt*) pageLeft;
                leftDataNode->rightSibPageNo = pageIdRight;

                for (int i = 0; i < INTARRAYLEAFSIZE; ++i) {
                    clearLeafNodeAtIdx(dataNode, i);
                    clearLeafNodeAtIdx(leftDataNode, i);
                }

                try {
                    bufMgr->unPinPage(file, pageIdLeft, true);
                } catch(PageNotPinnedException& e) {
                    // Do nothing.
                }

                path.push(pageIdRight);
                break;
            }

            // Read the next page that contains the next node 1 level deeper in the b-tree
            bufMgr->readPage(file, currNode->pageNoArray[idx], currPage);
            path.push(currNode->pageNoArray[idx]);

            // If the next level is the leaf level, set dataNode and break.
            // Otherwise, Set the current node and continue traversal
            if (currNode->level == 1) {
                dataNode = (LeafNodeInt*) currPage;
                break;
            } else {
                currNode = (NonLeafNodeInt*) currPage;
            }
        }

        // Checks if data node has space for the key to be inserted without creating node splits
        if (!insertKeyInLeafNode(dataNode, intKey, rid)) {

            // Split the leaf node and copy the middle key upwards in the b-tree
            PageId newPageId = splitLeafNode(dataNode, intKey, rid);

            try {
                bufMgr->unPinPage(file, path.top(), true);
            } catch(PageNotPinnedException& e) {
                // Do nothing.
            }
            path.pop();

            PageId currPageId = path.top();

            // Read the parent non-leaf node
            bufMgr->readPage(file, currPageId, currPage);
            try {
                bufMgr->unPinPage(file, currPageId, true);
            } catch (PageNotPinnedException& e) {
                // Do nothing.
            }
            currNode = (NonLeafNodeInt*) currPage;

            // Keep splitting parents until a parent has empty space available
            while (!insertKeyInNonLeafNode(currNode, intKey, newPageId)) {

                newPageId = splitNonLeafNode(currNode, intKey, newPageId);

                // Unpin the page before popping it from the stack
                try {
                    bufMgr->unPinPage(file, currPageId, true);
                } catch (PageNotPinnedException& e) {
                    // Do nothing.
                }
                path.pop();

                if (!path.empty()) {
                    currPageId = path.top();
                    bufMgr->readPage(file, currPageId, currPage);
                    currNode = (NonLeafNodeInt*) currPage;
                } else {
                    break;
                }
            }

            try {
                bufMgr->unPinPage(file, currPageId, true);
            } catch(PageNotPinnedException& e) {
                // Do nothing.
            }

            // No empty non-leaf node found, so create a new root
            if (path.empty()) {
                Page* rootPage;
                PageId pageId;

                // Allocate a new page for the root node
                bufMgr->allocPage(file, pageId, rootPage);

                // Create the new root node
                auto root = (NonLeafNodeInt*) rootPage;
                root->level = 0;

                for (int i = 1; i < INTARRAYNONLEAFSIZE; i++) {
                    clearNonLeafNodeAtIdx(root, i);
                }
                root->pageNoArray[INTARRAYNONLEAFSIZE] = Page::INVALID_NUMBER;

                // Copy the middle key and the page numbers of child nodes
                root->keyArray[0] = intKey;
                root->pageNoArray[0] = currPageId;
                root->pageNoArray[1] = newPageId;

                // Update the root page no of the b-tree
                rootPageNum = pageId;

                // Unpin the new root page and the newly split child node
                try {
                    bufMgr->unPinPage(file, newPageId, true);
                } catch (PageNotPinnedException& e) {
                    // Do nothing.
                }
                try {
                    bufMgr->unPinPage(file, pageId, true);
                } catch (PageNotPinnedException& e) {
                    // Do nothing.
                }
            }
            while (!path.empty()) {
                try {
                    bufMgr->unPinPage(file, path.top(), true);
                } catch(PageNotPinnedException& e) {
                    // Do nothing.
                }
                path.pop();
            }
        } else {
            while (!path.empty()) {
                try {
                    bufMgr->unPinPage(file, path.top(), true);
                } catch (PageNotPinnedException& e) {
                    // Do nothing.
                } catch (HashNotFoundException &e) {
                    // Do nothing.
                }
                path.pop();
            }
        }
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::splitLeafNode
    // -----------------------------------------------------------------------------
    PageId BTreeIndex::splitLeafNode(LeafNodeInt *dataNode, int& intKey, const RecordId rid) {
        // Create and allocate the page (and leaf node)
        Page* page;
        PageId pageId;
        bufMgr->allocPage(file, pageId, page);
        auto newLeafNode = (LeafNodeInt*) page;

        // Initialize the node with default values
        for (int i = 0; i < INTARRAYLEAFSIZE; i++)
            clearLeafNodeAtIdx(newLeafNode, i);

        // Get the middle index value and create sorted key and rid array
        int midIdx = (INTARRAYLEAFSIZE + 1) / 2;

        // Copy second half of data node to new leaf node and invalidate it in data node
        for (int i = midIdx; i < INTARRAYLEAFSIZE; ++i) {
            newLeafNode->keyArray[i-midIdx] = dataNode->keyArray[i];
            newLeafNode->ridArray[i-midIdx] = dataNode->ridArray[i];
            clearLeafNodeAtIdx(dataNode, i);
        }

        if (intKey < newLeafNode->keyArray[0])
            insertKeyInLeafNode(dataNode, intKey, rid);
        else
            insertKeyInLeafNode(newLeafNode, intKey, rid);

        // Update page IDs of right siblings
        newLeafNode->rightSibPageNo = dataNode->rightSibPageNo;
        dataNode->rightSibPageNo = pageId;

        intKey = newLeafNode->keyArray[0];

        // Unpin the newly split child node
        try {
            bufMgr->unPinPage(file, pageId, true);
        } catch (PageNotPinnedException& e) {
            // Do nothing.
        }

        return pageId;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::splitNonLeafNode
    // -----------------------------------------------------------------------------
    PageId BTreeIndex::splitNonLeafNode(NonLeafNodeInt* node, int &intKey, const PageId pageId) {
        // Create and allocate the page (and new node)
        Page* page;
        PageId pageId_;
        bufMgr->allocPage(file, pageId_, page);
        auto newNode = (NonLeafNodeInt*) page;

        // Initialize the node with default values
        for (int i = 0; i < INTARRAYNONLEAFSIZE; i++) {
            clearNonLeafNodeAtIdx(newNode, i);
        }
        newNode->pageNoArray[INTARRAYNONLEAFSIZE] = Page::INVALID_NUMBER;

        // Get the middle index value and create sorted key and rid array
        int midIdx = (INTARRAYNONLEAFSIZE + 1) / 2, prevKey = INT_MIN, i, j;
        int keyArr[INTARRAYNONLEAFSIZE+1];
        PageId pageNoArr[INTARRAYNONLEAFSIZE+2];

        // The first page number won't be changed during a split as we always
        // create the new leaf (or non-leaf) node to the right side of the
        // existing node.
        pageNoArr[0] = node->pageNoArray[0];

        // Create a sorted array of all keys with new key in its position
        for (i = 0, j = 0; j < INTARRAYNONLEAFSIZE; i++) {
            if (prevKey <= intKey && intKey < node->keyArray[j]) {
                keyArr[i] = intKey;
                pageNoArr[i+1] = pageId;
                prevKey = node->keyArray[j];
                continue;
            }
            prevKey = keyArr[i] = node->keyArray[j];
            pageNoArr[i+1] = node->pageNoArray[j+1];
            j++;
        }
        // Special case where the key is the last key in the sorted key list
        if (i == j) {
            keyArr[i] = intKey;
            pageNoArr[i+1] = pageId;
        }

        node->pageNoArray[0] = pageNoArr[0];
        // Update keys of dataNode (left split) to the first half of keys
        for (i = 0; i < midIdx; ++i) {
            node->keyArray[i] = keyArr[i];
            node->pageNoArray[i+1] = pageNoArr[i+1];
        }

        newNode->pageNoArray[0] = pageNoArr[midIdx+1];
        // Update keys of newNode (right split) with second half of keys
        for (i = midIdx; i < INTARRAYNONLEAFSIZE; ++i) {
            newNode->keyArray[i-midIdx] = keyArr[i+1];
            newNode->pageNoArray[i-midIdx+1] = pageNoArr[i+2];
            // Invalidate corresponding indices in node as second half of that
            // array is now empty
            clearNonLeafNodeAtIdx(node, i);
            clearNonLeafNodeAtIdx(newNode, i-1);
        }
        node->pageNoArray[INTARRAYNONLEAFSIZE] = Page::INVALID_NUMBER;

        newNode->level = node->level;

        intKey = keyArr[midIdx];

        // Unpin the newly split child node
        try {
            bufMgr->unPinPage(file, pageId_, true);
        } catch (PageNotPinnedException& e) {
            // Do nothing.
        }

        return pageId_;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::insertKeyInLeafNode
    // -----------------------------------------------------------------------------
    bool BTreeIndex::insertKeyInLeafNode(LeafNodeInt *node, int key, RecordId rid) {
        // Checks if the node contains any empty space for insertion
        if (node->ridArray[INTARRAYLEAFSIZE-1].page_number != Page::INVALID_NUMBER)
            return false;

        int idx, newKey = key;
        RecordId newRid = rid;

        // Find the index to insert the key-record pair
        for (idx = 0;
             idx < INTARRAYLEAFSIZE &&
             node->ridArray[idx].page_number != Page::INVALID_NUMBER &&
             node->keyArray[idx] < key;
             idx++);

        // Insert the key at position idx and shift everything else right
        for (; node->ridArray[idx].page_number != Page::INVALID_NUMBER; idx++) {
            int oldKey = node->keyArray[idx];
            RecordId oldRid = node->ridArray[idx];
            node->keyArray[idx] = newKey;
            node->ridArray[idx] = newRid;
            newKey = oldKey;
            newRid = oldRid;
        }
        node->keyArray[idx] = newKey;
        node->ridArray[idx] = newRid;

        return true;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::insertKeyInNonLeafNode
    // -----------------------------------------------------------------------------
    bool BTreeIndex::insertKeyInNonLeafNode(NonLeafNodeInt* node, int key, PageId pageId) {
        // Checks if the node contains any empty space for insertion
        if (node->pageNoArray[INTARRAYNONLEAFSIZE] != Page::INVALID_NUMBER)
            return false;

        int idx, newKey = key;
        PageId newPageId = pageId;

        // Find the index to insert the key-pageId pair
        for (idx = 0;
             idx < INTARRAYNONLEAFSIZE &&
             node->pageNoArray[idx+1] != Page::INVALID_NUMBER &&
             node->keyArray[idx] < key;
             idx++);

        // Insert the key at position idx and shift everything else right
        for (; node->pageNoArray[idx+1] != Page::INVALID_NUMBER; idx++) {
            int oldKey = node->keyArray[idx];
            PageId oldPageId = node->pageNoArray[idx+1];
            node->keyArray[idx] = newKey;
            node->pageNoArray[idx+1] = newPageId;
            newKey = oldKey;
            newPageId = oldPageId;
        }
        node->keyArray[idx] = newKey;
        node->pageNoArray[idx+1] = newPageId;

        return true;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::clearLeafNodeAtIdx
    // -----------------------------------------------------------------------------
    void BTreeIndex::clearLeafNodeAtIdx(LeafNodeInt* node, int idx) {
        node->keyArray[idx] = -1;
        node->ridArray[idx].page_number = Page::INVALID_NUMBER;
        node->ridArray[idx].slot_number = Page::INVALID_SLOT;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::clearNonLeafNodeAtIdx
    // -----------------------------------------------------------------------------
    void BTreeIndex::clearNonLeafNodeAtIdx(NonLeafNodeInt* node, int idx) {
        node->keyArray[idx] = -1;
        node->pageNoArray[idx] = Page::INVALID_NUMBER;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::startScan
    // -----------------------------------------------------------------------------
    void BTreeIndex::startScan(const void* lowValParm,
                               const Operator lowOpParm,
                               const void* highValParm,
                               const Operator highOpParm) {
        // Verify expected op values
        if ((lowOpParm != GT && lowOpParm != GTE) || (highOpParm != LT && highOpParm != LTE)) {
            throw BadOpcodesException();
        }

        lowValInt = *(int *)lowValParm;
        highValInt = *(int *)highValParm;

        // Verify bounds
        if (lowValInt > highValInt)
            throw BadScanrangeException();

        if (scanExecuting) {
            endScan();
        }

        // Set up variables for scan
        scanExecuting = true;
        lowOp = lowOpParm;
        highOp = highOpParm;

        // Scan the tree from root to find the parent of the first leaf node to be scanned
        getFirstParent(rootPageNum);
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::getFirstParent
    // -----------------------------------------------------------------------------
    void BTreeIndex::getFirstParent(PageId pageNum) {
        currentPageNum = pageNum;
        bufMgr->readPage(file, currentPageNum, currentPageData);
        auto nonLeafNode = (NonLeafNodeInt*) currentPageData;

        int i = 0;
        while (i < INTARRAYNONLEAFSIZE
               && lowValInt >= nonLeafNode->keyArray[i]
               && nonLeafNode->pageNoArray[i+1] != Page::INVALID_NUMBER)
            i++;

        // A level above leaf node
        if (nonLeafNode->level == 1) {
            try {
                bufMgr->unPinPage(file, currentPageNum, false);
            } catch (PageNotPinnedException& e) {
                // Do nothing.
            }

            // Search for the key in leaf node
            currentPageNum = nonLeafNode->pageNoArray[i];
            bufMgr->readPage(file, currentPageNum, currentPageData);

            // Use binary search to set the value of nextEntry to read the first record that is in the scan range
            auto currentNode = (LeafNodeInt*) currentPageData;
            int low = 0, high = INTARRAYLEAFSIZE - 1;
            int mid;
            while (low <= high) {
              mid  = (low + high) / 2;

              if (currentNode->ridArray[mid].page_number == Page::INVALID_NUMBER) {
                high = mid - 1;
              } else if ((lowOp == GT && currentNode->keyArray[mid] == lowValInt + 1) ||
                  (lowOp == GTE && currentNode->keyArray[mid] == lowValInt)) {
                break;
              } else if ((lowOp == GT && currentNode->keyArray[mid] <= lowValInt) ||
                  (lowOp == GTE && currentNode->keyArray[mid] < lowValInt)) {
                low = mid + 1;
              } else {
                high = mid - 1;
              }
            }
            nextEntry = mid;
        } else {
            // No record found here, unpin page and move on to the next page
            try {
                bufMgr->unPinPage(file, currentPageNum, false);
            } catch (PageNotPinnedException& e) {
                // Do nothing.
            }
            getFirstParent(nonLeafNode->pageNoArray[i]);
        }
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::scanNext
    // -----------------------------------------------------------------------------
    void BTreeIndex::scanNext(RecordId& outRid) {
        // Check that scan has successfully started
        if (!scanExecuting)
            throw ScanNotInitializedException();

        // Keep track of node being evaluated
        auto currentNode = (LeafNodeInt*) currentPageData;

        // Look for record id of next matching tuple
        while (true) {
            // Validate index of entry to be evaluated
            if (nextEntry == INTARRAYLEAFSIZE) {
                // Unpin page since no more entries to be scanned on this leaf page
                try {
                    bufMgr->unPinPage(file, currentPageNum, false);
                } catch (PageNotPinnedException& e) {
                    // Do nothing.
                }

                // Move to right sibling leaf page
                PageId rightSibPageNo = currentNode->rightSibPageNo;

                // Check that the right sibling is a valid leaf page
                if (rightSibPageNo == Page::INVALID_NUMBER)
                    // No more entries to be scanned.
                    throw IndexScanCompletedException();

                // Update the parameters for the index since leaf page is invalid
                nextEntry = 0;
                currentPageNum = rightSibPageNo;
                bufMgr->readPage(file, currentPageNum, currentPageData);
                currentNode = (LeafNodeInt*) currentPageData;
            }

            if (currentNode->ridArray[nextEntry].page_number == Page::INVALID_NUMBER) {
                nextEntry = INTARRAYLEAFSIZE;
                continue;
            }

            // Check lower limit of scan with entry key. Skip entry if too small.
            if ((lowOp == GT && currentNode->keyArray[nextEntry] <= lowValInt) ||
                (lowOp == GTE && currentNode->keyArray[nextEntry] < lowValInt)) {
                nextEntry++;
                // Restart loop to process next entry
                continue;
            }

            // Check upper limit of scan with entry key. Scan is complete if too big.
            if ((highOp == LT && currentNode->keyArray[nextEntry] >= highValInt)
                || (highOp == LTE && currentNode->keyArray[nextEntry] > highValInt))
                throw IndexScanCompletedException();

            // Exit loop since an entry that meets the requirements has been found
            break;
        }

        // Return the record ID of the entry
        outRid = currentNode->ridArray[nextEntry];

        // Update the index of the next entry to be scanned
        nextEntry++;
    }


    // -----------------------------------------------------------------------------
    // BTreeIndex::endScan
    // -----------------------------------------------------------------------------
    //
    void BTreeIndex::endScan() {
        // Make sure that a scan is successfully executing
        if (!scanExecuting)
            throw ScanNotInitializedException();

        // Terminate the current scan
        scanExecuting = false;

        // Unpin the pages that are currently pinned
        try {
            bufMgr->unPinPage(file, currentPageNum, false);
        } catch (PageNotPinnedException& e) {
            // Do nothing.
        } catch (HashNotFoundException& e) {
            // Do nothing.
        }
    }

}
