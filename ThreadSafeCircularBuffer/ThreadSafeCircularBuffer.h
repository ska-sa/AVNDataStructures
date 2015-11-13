#ifndef THREAD_SAFE_CIRCULAR_BUFFER_H
#define THREAD_SAFE_CIRCULAR_BUFFER_H

//System includes
#include <vector>
#include <iostream>

#ifdef _WIN32
#include <stdint.h>
typedef unsigned __int64 uint64_t;
#else
#include <inttypes.h>
#endif

//Library includes
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>

using namespace std;

template<class T> struct cBufferElement
{
    std::vector<T>          m_vTElement;
    uint32_t                m_u32DataStartIndex;
    uint32_t                m_u32DataSize;

    void resize(uint32_t u32NSamples)
    {
        m_vTElement.resize(u32NSamples);
    }

    //The following 3 functions can optionally be used if fractions of the complete element object represented by this class are to be used.

    //If the data in the element has been cleared signal with this function
    void setEmpty()
    {
        m_u32DataSize = 0;
        m_u32DataStartIndex = 0;
    }

    //If data has been added or removed from the vector in this element
    //update its span with this function
    void setDataSpan(uint32_t u32StartIndex, uint32_t u32Size)
    {
        m_u32DataSize = u32Size;
        m_u32DataStartIndex = u32StartIndex;
    }

    //If the element has been filled signal with this function
    void setFull()
    {
        m_u32DataSize = m_vTElement.size();
        m_u32DataStartIndex = 0;
    }

    //If data from the front the buffer has been consumed.
    //Increase u32StartIndex and decrement the size
    void setDataUsed(uint32_t u32Size)
    {
        m_u32DataSize -= u32Size;
        m_u32DataStartIndex += u32Size;
    }

    //If data has been added to the end of the buffer
    void setDataAdded(uint32_t u32Size)
    {
        m_u32DataSize += u32Size;
    }

    T* getDataStartPointer()
    {
        return &m_vTElement[m_u32DataStartIndex];
    }

    T* getDataPointerAtIndex(uint32_t u32Index)
    {
        return &m_vTElement[u32Index];
    }

    unsigned short allocationSize()
    {
        return m_vTElement.size();
    }

    unsigned short dataSize()
    {
        return m_u32DataSize;
    }
};

//Class for using a circular buffer between producer and consumer threads. Note the current implementation
//is only designed for a single producer and single consumer.

template<class T> class cThreadSafeCircularBuffer
{
public:    
    cThreadSafeCircularBuffer(uint32_t u32NElements, uint32_t u32ElementSize) :
        m_u32ReadIndex(0),
        m_u32WriteIndex(0),
        m_u32Level(0)
    {
        resize(u32NElements, u32ElementSize);
    }

    ~cThreadSafeCircularBuffer()
    {
        //Prevent any threads from waiting on condition variables after this class is destructed
        m_oConditionReadPossible.notify_all();
        m_oConditionWritePossible.notify_all();

        //Allow some time for condition variables to be triggered and unlocked before dellocating them.
        boost::this_thread::sleep(boost::posix_time::milliseconds(200));
    }

    int32_t getNextReadIndex(uint32_t u32Timeout_ms = UINT_MAX)
    {
        int32_t i32NextReadIndex;

        {
            boost::unique_lock<boost::mutex> oLock(m_oMutex);
            if(!m_u32Level)
            {
                if(!m_oConditionReadPossible.timed_wait(oLock, boost::posix_time::milliseconds(u32Timeout_ms)) )
                    return -1;
            }

            i32NextReadIndex = m_u32ReadIndex;
        }

        return i32NextReadIndex;
    }

    int32_t getNextWriteIndex(unsigned long u32Timeout_ms = UINT_MAX)
    {
        int32_t i32NextWriteIndex;
        {
            boost::unique_lock<boost::mutex> oLock(m_oMutex);
            if(m_u32Level >= m_vTCircularBuffer.size())
            {
                if(!m_oConditionWritePossible.timed_wait(oLock, boost::posix_time::milliseconds(u32Timeout_ms)) )
                {
                    return -1;
                }
            }

            i32NextWriteIndex = m_u32WriteIndex ;
        }

        return i32NextWriteIndex;
    }

    int32_t tryToGetNextWriteIndex()
    {
        int32_t i32NextWriteIndex;
        {
            boost::unique_lock<boost::mutex> oLock(m_oMutex);
            if(m_u32Level >= m_vTCircularBuffer.size())
            {
                return -1;
            }

            i32NextWriteIndex = m_u32WriteIndex ;
        }

        return i32NextWriteIndex;
    }

    void elementRead()
    {
        boost::unique_lock<boost::mutex> oLock(m_oMutex);
        
        //Clear the element contents
        getElementPointer(m_u32ReadIndex)->setEmpty();
            
        m_u32Level--;
        m_u32ReadIndex++;

        if(m_u32ReadIndex >= m_vTCircularBuffer.size())
            m_u32ReadIndex = 0;

        if(m_u32Level == m_vTCircularBuffer.size() - 1)
        {
            m_oConditionWritePossible.notify_one();
        }
    }

    void elementWritten()
    {
        //Assume that the element has been completely filled.
        boost::unique_lock<boost::mutex> oLock(m_oMutex);
        m_u32Level++;
        m_u32WriteIndex++;

        if(m_u32WriteIndex >= m_vTCircularBuffer.size())
            m_u32WriteIndex = 0;

        if(m_u32Level == 1)
        {
            m_oConditionReadPossible.notify_one();
        }
    }

    uint32_t getLevel()
    {
        boost::unique_lock<boost::mutex> oLock(m_oMutex);
        return m_u32Level;
    }

    uint32_t getElementSize()
    {
        boost::unique_lock<boost::mutex> oLock(m_oMutex);
        if(!m_vTCircularBuffer.size())
            return 0;

        return m_vTCircularBuffer[0].size();
    }

    uint32_t getNElements()
    {
        boost::unique_lock<boost::mutex> oLock(m_oMutex);
        return m_vTCircularBuffer.size();
    }


    void resize(uint32_t nElements, uint32_t uiElementSize)
    {
        boost::unique_lock<boost::mutex> oLock(m_oMutex);
        m_vTCircularBuffer.resize(nElements);
        m_vi64Timestamps_us.resize(nElements);

        for(uint32_t ui = 0; ui < m_vTCircularBuffer.size(); ui++)
        {
            m_vTCircularBuffer[ui].resize(uiElementSize);
        }
    }

    T *getElementDataPointer(uint32_t uiIndex)
    {
        return m_vTCircularBuffer[uiIndex].getDataStartPointer();
    }

    cBufferElement<T> *getElementPointer(uint32_t uiIndex)
    {
        return &m_vTCircularBuffer.front() + uiIndex;
    }

    int64_t getElementTimestamp_us(uint32_t uiIndex)
    {
        return m_vi64Timestamps_us[uiIndex];
    }

    void setElementTimestamp(uint32_t uiIndex, int64_t i64Timestamp_us)
    {
        m_vi64Timestamps_us[uiIndex] = i64Timestamp_us;
    }

    void clear()
    {
        boost::unique_lock<boost::mutex> oLock(m_oMutex);
        m_u32ReadIndex = 0;
        m_u32WriteIndex = 0;
        m_u32Level = 0;

        for(uint32_t ui =0; ui < m_vTCircularBuffer.size(); ui++)
        {
            m_vTCircularBuffer[ui].setEmpty();
            m_vi64Timestamps_us[ui] = 0;
        }

        m_oConditionWritePossible.notify_one();
    }


private:
    std::vector<cBufferElement<T> > m_vTCircularBuffer;
    std::vector<int64_t>            m_vi64Timestamps_us;
    uint32_t                        m_u32ReadIndex;   //Index of an existing element that can be read
    uint32_t                        m_u32WriteIndex;  //Index of an empty element that can be written to
    uint32_t                        m_u32Level;
    boost::mutex                    m_oMutex;
    boost::condition_variable       m_oConditionReadPossible;
    boost::condition_variable       m_oConditionWritePossible;
};

#endif // THREAD_SAFE_CIRCULAR_BUFFER_H
