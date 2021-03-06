#include "dhudpdecoder.h"

namespace nProtocUDP{

DHudpDecoder::DHudpDecoder(DHudpRcvQueue &q, QObject *parent) :
    QObject(parent),i_queue(q),i_wrongFragsCounter(0),i_rcv_cyc(0),
    i_lastCorrectionFromCyc(0),i_lastCorrectionToCyc(0),
    i_correctionCycCounter(0),i_wrongFragsLimit(WRONG_FRAGS_LIMIT_DEFAULT)
{
    //cache file
    i_rcvCacheFileInfo.setFile(RCVER_CACHE_FILE);
    if(i_rcvCacheFileInfo.exists()){
        QFile::remove(i_rcvCacheFileInfo.filePath());
    }
    if(!touch(i_rcvCacheFileInfo.filePath()))
        qDebug() << "\t Error: failed to touch receive cache file";

    this->touch(i_rcvCacheFileInfo.fileName() + ".raw");
}

void DHudpDecoder::resetDecodeParameters(const DecParams &p)
{
    qDebug() << "DHudpDecoder::setDecodeParameters()";
    i_params = p;
    i_wrongFragsLimit = i_params.inBlockDataSize / i_params.fragSize +1;
    initRcvBitMapFromBlocksNum(i_params.totalEncBlocks);
    //prepare for cycle 0
    i_rcv_cyc = 0;
    this->clearRcvBlocksCacheForCycle(0);
}

void DHudpDecoder::processQueue()
{
    if( !isParamsReady() ){
        qDebug() << "DHudpDecoder::processQueue()"
                 << "Err: parameter not ready";
        return;
    }

    while(!i_queue.isEmpty()){
        processFragment(i_queue.waitForDequeue());
    }
}

bool DHudpDecoder::isParamsReady()
{
    return ( 0 != i_params.oneCycleBlockNum )
            && ( 0 != i_params.totalEncBlocks )
            && ( 0 != i_params.totalCycleNum );
}

void DHudpDecoder::initRcvBitMapFromBlocksNum(quint64 bn)
{
    i_rcvBitMap.resize(bn);
    i_rcvBitMap.fill(false);
    i_gotBlockSNs.clear();
}

void DHudpDecoder::clearRcvBlocksCacheForCycle(quint32 cyc)
{
    qDebug() << "DHudpDecoder::clearRcvBlocksCacheForCycle()" << cyc;
    i_rcvCycleBlocks.clear();

    quint32 blockNum  = blockNumInCycle(cyc);
    if( 0 == blockNum ){
        qDebug() << "\t Err: block number in this cycle is 0 !";
        return;
    }

    for(int i = 0; i< blockNum; ++i){
        i_rcvCycleBlocks.append(
                    RcvBlock(cyc,
                             i,
                             (i_params.inBlockCoeffLen + i_params.inBlockDataSize)));
    }
}

bool DHudpDecoder::processFragment(const QByteArray &a)
{
    Fragment frag;
    if( 0 == frag.fromArray(a))
        return false;

//    qDebug() << "DHudpDecoder::processFragment()" << frag.dbgString();

    //filter fragmets
    if( frag.cyc != i_rcv_cyc ){
//        qDebug() << "DHudpDecoder::processFragment()"
//                 << "wrong frag of cyc " << frag.cyc
//                 << " / rcv_cyc " << i_rcv_cyc
//                 << "\t\t counter=" << i_wrongFragsCounter;
        ++i_wrongFragsCounter;
        this->correctCycleTo(i_rcv_cyc);
        return false;
    }else{
        i_wrongFragsCounter = 0;
        i_correctionCycCounter = 0;
    }

    //assemble fragment into block
    if(i_rcvCycleBlocks.isEmpty()){
        qDebug() << "\t Err: rcv blocks cache list empty!";
        return false;
    }

    RcvBlock &b = i_rcvCycleBlocks[frag.blockNo];
    if( ! b.isComplete() ){
        b.assembleFragment(frag);
        if( b.isComplete() ){ //got this whole block
            qDebug() << "[cached Block] " << b.dbgString();
            emit sig_gotBlockSN(i_params.oneCycleBlockNum*b.cyc + b.blockNo);

            //check if got all current cycle blocks
            if( this->checkCurrentCycleBlocks())
                this->onGotAllCurrentCycleBlocks();
        }
    }

    return true;
}

/* Important: can be called repeatly, without side effect
 * this func is used to avoid massive sending CON_CHG_CYC
 */
void DHudpDecoder::correctCycleTo(quint32 cyc)
{
    if( i_rcv_cyc == i_lastCorrectionFromCyc
            &&  cyc == i_lastCorrectionToCyc)
        return;

    if( i_wrongFragsCounter > i_wrongFragsLimit){
        i_wrongFragsCounter = 0;
        emit sig_correctionCyc(i_rcv_cyc);
        i_correctionCycCounter++;
    }

    if( i_correctionCycCounter > CORRECTION_CYC_TIMES_LIMIT){
        qDebug() << "DHudpDecoder::correctCycleTo()"
                 << cyc << "/" << i_rcv_cyc
                 << "\t correction failed, sth. maybe wrong!!!";
    }

    i_lastCorrectionFromCyc = i_rcv_cyc;
    i_lastCorrectionToCyc = cyc;
}

bool DHudpDecoder::checkCurrentCycleBlocks()
{
    qDebug() << "DataHandler::checkCurrentCycleBlocks()"
             << "cycle" << i_rcv_cyc;

    quint64 tgtNum = blockNumInCycle(i_rcv_cyc);
    qDebug() << "\t tgt" << tgtNum << "blocks";
    if( i_rcvCycleBlocks.size() != tgtNum)
        return false;

    for(int i = 0; i < i_rcvCycleBlocks.size(); ++i){
        if( !i_rcvCycleBlocks[i].isComplete() ){
            qDebug() << "\t block" << i << " not complete"
                        << i_rcvCycleBlocks[i].dbgString();
            return false;
        }
    }

    qDebug() << "\t Got all : current cycle blocks.";
    return true;
}

void DHudpDecoder::onGotAllCurrentCycleBlocks()
{
    this->saveCurrentCycleBlocks();

    //check if all saved
    for(int i = 0 ; i< i_rcvBitMap.size() ; ++i){
        if(!i_rcvBitMap.testBit(i)){    //if a block is absence
            //found its cycle no.
            quint32 tgtCycle = (i+1) / i_params.oneCycleBlockNum;
            //cmd server to send that cycle
            if(tgtCycle > i_rcv_cyc){
                this->toCycle(tgtCycle);
                emit sig_needNextCycle();
            }else{
                this->toCycle(tgtCycle);
                this->correctCycleTo(tgtCycle);
            }
            return;
        }
    }

    //all file is saved
    emit sig_fullFileSaved();

    if(! this->testDecode()){
        qDebug() << "[DHudp] test decode failed";
    }

    i_queue.waitForClear();
}

bool DHudpDecoder::saveCurrentCycleBlocks()
{
    qDebug() << "DataHandler::saveCurrentCycleBlocks()"
             << "cyc" << i_rcv_cyc
             << "to" << i_rcvCacheFileInfo.fileName();

    QFile f(i_rcvCacheFileInfo.filePath());

    if( !f.open(QIODevice::WriteOnly | QIODevice::Append)){
        qDebug() << "\t error open rcv cache file";
        f.close();
        return false;
    }

    quint64 dataOffset = 0;
    quint64 wroteBytes = 0;
    int wResult = 0;

    for(int i = 0; i< i_rcvCycleBlocks.size(); ++i){
        const RcvBlock &b = i_rcvCycleBlocks.at(i);
        dataOffset = b.tgtSize * (i_params.oneCycleBlockNum * b.cyc + b.blockNo);
        f.seek(dataOffset);
        wResult = f.write(b.data.data(), b.tgtSize);
        if( -1 == wResult ){
            qDebug() << "\t write error" << f.errorString();
            break;
        }else {
            wroteBytes += wResult;
            this->markSavedBlock(b);
            qDebug() << "\t saved block" << b.dbgString();
            qDebug() << "\t wrote out" << wroteBytes << "bytes";
        }
    }

    f.close();
    return true;
}

void DHudpDecoder::toCycle(quint32 cyc)
{
    if( cyc >= i_params.totalCycleNum ) return;

    i_rcv_cyc = cyc ;
    this->clearRcvBlocksCacheForCycle(i_rcv_cyc);
}

void DHudpDecoder::markSavedBlock(const RcvBlock &b)
{
    quint32 blockSN = i_params.oneCycleBlockNum * b.cyc + b.blockNo ;
    i_rcvBitMap.setBit(blockSN, true);
    i_gotBlockSNs.insert(blockSN);

    emit sig_progressPercent(i_gotBlockSNs.size()*100/ i_params.totalEncBlocks);
}

quint32 DHudpDecoder::blockNumInCycle(quint32 cyc) const
{
    quint32 n;
    if( cyc + 1 < i_params.totalCycleNum
            || 0 == i_params.totalEncBlocks % i_params.oneCycleBlockNum){
         n = i_params.oneCycleBlockNum;
    }else if( cyc +1 == i_params.totalCycleNum){
        n = i_params.totalEncBlocks % i_params.oneCycleBlockNum;
    }else {
        n = 0;
    }
    return n;
}

bool DHudpDecoder::touch(QString aFilePath)
{
    if( QFile::exists(aFilePath)) return true;

    QFile f(aFilePath);
    bool rst = f.open(QIODevice::ReadWrite);
    f.close();
    return rst;
}

bool DHudpDecoder::testDecode()
{
    qDebug() << "DHudpDecoder::testDecode()";
    QFile encFile(RCVER_CACHE_FILE);
    QFile rawFile(DECODE_TO_RAW_FILE);

    if(!encFile.open(QIODevice::ReadOnly)){
        qDebug() << "\t error open encoded file";
        return false;
    }

    rawFile.remove();
    if( !rawFile.exists() && !this->touch(rawFile.fileName())){
        qDebug() << "\t failed create tgt raw file";
        return false;
    }

    if(!rawFile.open(QIODevice::ReadWrite)){
        qDebug() << "\t error open tgt raw file to write";
        return false;
    }

    quint32 coeffLen = i_params.inBlockCoeffLen;
    quint32 dataLen = i_params.inBlockDataSize;
    quint32 decodeUintSize = coeffLen + dataLen;   //uint[coeff | data]

    quint64 rawFileSize = i_params.rawFileLength;
    quint64 totalUnitsNum = i_params.totalEncBlocks;
    int wroteOutBytes = 0;

    for(quint64 i = 0 ; i< totalUnitsNum; ++i){
        QByteArray unit = encFile.read(decodeUintSize);
        if(unit.size() != decodeUintSize){
            qDebug() << "\t error read unit" << i;
            encFile.close();
            rawFile.close();
            return false;
        }

        //TODO: decode
        QByteArray rawBlock = unit.right(dataLen);

        //write out
        if( wroteOutBytes + rawBlock.size() > rawFileSize)
            rawBlock.resize(rawFileSize - wroteOutBytes);   //last block

        wroteOutBytes += rawFile.write(rawBlock);

    }
    if(wroteOutBytes == rawFileSize){
        encFile.close();
        rawFile.close();
        return true;
    }else {
        qDebug() << "\t wrong raw file size";
        encFile.close();
        rawFile.close();
        return false;
    }

}

}
