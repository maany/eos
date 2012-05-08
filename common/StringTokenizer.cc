// ----------------------------------------------------------------------
// File: StringTokenizer.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/


/*----------------------------------------------------------------------------*/
#include "common/StringTokenizer.hh"
/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
/** 
 * Constructor
 * 
 */
/*----------------------------------------------------------------------------*/
StringTokenizer::StringTokenizer(const char* s) 
{
  // -----------------------------------------------------------
  // the constructor just parses lines not token's within a line
  // -----------------------------------------------------------

  if (s) {
    fBuffer = strdup(s);
  } else {
    fBuffer = 0;
    return;
  }

  
  bool inquote=false;
  if (fBuffer[0] != 0) {
    // set the first pointer to offset 0
    fLineStart.push_back(0);
  }
  // intelligent parsing considering quoting
  for (size_t i=0; i< strlen(fBuffer); i++) {
    if (fBuffer[i] == '"') {
      if (inquote) 
	inquote=false;
      else
	inquote=true;
    }
    if ( (!inquote) && fBuffer[i] == '\n') {
      fLineStart.push_back(i);
    }
  }
  fCurrentLine=-1;
  fCurrentArg=-1;
}

/*----------------------------------------------------------------------------*/
/** 
 * Destructor
 * 
 */
/*----------------------------------------------------------------------------*/
StringTokenizer::~StringTokenizer()
{
  if (fBuffer) {
    free(fBuffer);
    fBuffer = 0;
  }
}

/** 
 * Return the next parsed line
 * 
 * 
 * @return char reference to the next line
 */
const char* 
StringTokenizer::GetLine()
{
  fCurrentLine++;
  if (fCurrentLine < (int)fLineStart.size()) {
    char* line = fBuffer+fLineStart[fCurrentLine];
    char* wordptr = line;
    bool inquote=false;

    if (line[0]!=0) 
fLineArgs.push_back(0);

    for (size_t i=0; i< strlen(line); i++) {
      if (line[i] == '"') {
	if (inquote) {
	  inquote = false;
	} else {
	  inquote = true;
	}	  
	
	if (line[i] == ' ') {
	  if (!inquote) {
	    if ( (i>1) && (line[i] == '\\') ) {
	      // don't start a new word here
	    } else {
	      line[i] = 0;
	      fLineArgs.push_back(wordptr);
	      // start a new word here
	      wordptr = line+i+1;
	    }
	  }
	}
      }
      if (line[i] == '\n') {
	line[i] = 0;
      }
    }
    return line;
  } else {
    return 0;
  }
}


/** 
 * Return next parsed space seperated token taking into account escaped blanks and quoted strings
 * 
 * 
 * @return char reference to the next argument token
 */
const char* 
StringTokenizer::GetToken()
{
  fCurrentArg++;
  if (fCurrentArg < (int)fLineArgs.size()) {
    return fLineArgs[fCurrentArg].c_str();
  } else {
    return 0;
  }
}

/*----------------------------------------------------------------------------*/
EOSCOMMONNAMESPACE_END
