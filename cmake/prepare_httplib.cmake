if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "prepare_httplib.cmake requires INPUT and OUTPUT")
endif()

file(READ "${INPUT}" HTTPLIB_SOURCE)
set(UNSAFE_RETRY [=[  if (error == Error::SSLPeerCouldBeClosed_) {
    assert(!ret);
    ret = send_(req, res, error);
  }]=])
set(SAFE_NO_RETRY [=[  // Do not replay a request after bytes may have been sent. The
  // extension's caller classifies this outcome and decides whether a retry is
  // safe for the particular HTTP method.
]=])

string(FIND "${HTTPLIB_SOURCE}" "${UNSAFE_RETRY}" UNSAFE_RETRY_OFFSET)
if(UNSAFE_RETRY_OFFSET EQUAL -1)
  message(FATAL_ERROR "The pinned httplib source no longer contains the expected implicit retry")
endif()

string(REPLACE "${UNSAFE_RETRY}" "${SAFE_NO_RETRY}" HTTPLIB_SOURCE "${HTTPLIB_SOURCE}")
file(WRITE "${OUTPUT}" "${HTTPLIB_SOURCE}")
