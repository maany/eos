//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   Metadata exception
//------------------------------------------------------------------------------

#ifndef EOS_NS_MD_EXCEPTION_HH
#define EOS_NS_MD_EXCEPTION_HH

#include <stdexcept>
#include <sstream>
#include <cerrno>
#include <cstring>

namespace eos
{
  //----------------------------------------------------------------------------
  //! Metadata exception
  //----------------------------------------------------------------------------
  class MDException: public std::exception
  {
  public:
    //------------------------------------------------------------------------
    // Constructor
    //------------------------------------------------------------------------
    MDException( int errorNo = ENODATA ) throw():
      pErrorNo( errorNo ), pTmpMessage( 0 ) {}

    //------------------------------------------------------------------------
    //! Destructor
    //------------------------------------------------------------------------
    virtual ~MDException() throw()
    {
      delete [] pTmpMessage;
    }

    //------------------------------------------------------------------------
    //! Copy constructor - this is actually required because we cannot copy
    //! stringstreams
    //------------------------------------------------------------------------
    MDException( MDException &e )
    {
      pMessage << e.getMessage().str();
      pErrorNo = e.getErrno();
      pTmpMessage = 0;
    }

    //------------------------------------------------------------------------
    //! Get errno assosiated with the exception
    //------------------------------------------------------------------------
    int getErrno() const
    {
      return pErrorNo;
    }

    //------------------------------------------------------------------------
    //! Get the message stream
    //------------------------------------------------------------------------
    std::ostringstream &getMessage()
    {
      return pMessage;
    }

    //------------------------------------------------------------------------
    // Get the message
    //------------------------------------------------------------------------
    virtual const char *what() const throw()
    {
      // we could to that instead: return (pMessage.str()+" ").c_str();
      // but it's ugly and probably not portable

      if( pTmpMessage )
        delete [] pTmpMessage;

      std::string msg = pMessage.str();
      pTmpMessage = new char[msg.length()+1];
      pTmpMessage[msg.length()] = 0;
      strcpy( pTmpMessage, msg.c_str() );
      return pTmpMessage;
    }

  private:
    //------------------------------------------------------------------------
    // Data members
    //------------------------------------------------------------------------
    std::ostringstream  pMessage;
    int                 pErrorNo;
    mutable char       *pTmpMessage;
  };
}

#endif // EOS_NS_MD_EXCEPTION_HH
