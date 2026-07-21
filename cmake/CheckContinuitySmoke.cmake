file(MAKE_DIRECTORY "${OUTDIR}")
execute_process(
  COMMAND "${AGENTPDF}" convert "${PDF}" -o "${OUTDIR}"
  RESULT_VARIABLE convert_result
  OUTPUT_VARIABLE convert_stdout
  ERROR_VARIABLE convert_stderr
)
if(NOT convert_result EQUAL 0)
  message(FATAL_ERROR "conversion failed: ${convert_stderr}")
endif()

file(READ "${OUTDIR}/ccp_addis2000.md" markdown)
string(LENGTH "${markdown}" markdown_length)
if(markdown_length LESS 30000)
  message(FATAL_ERROR "Addis continuity output is unexpectedly short: ${markdown_length}")
endif()
string(FIND "${markdown}" "A National Survey of Practicing Psychologists" title_position)
string(FIND "${markdown}" "There has been considerable debate" body_position)
if(title_position EQUAL -1 OR body_position EQUAL -1)
  message(FATAL_ERROR "Addis title/body continuity anchors are missing")
endif()
