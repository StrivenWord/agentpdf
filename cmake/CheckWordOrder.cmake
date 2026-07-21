# Word-order golden check for agentpdf outputs.
# Expects: AGENTPDF, PDF, GOLD, OUTDIR
# Optional: HEUR (path to heuristics JSON)

if(NOT AGENTPDF OR NOT PDF OR NOT GOLD OR NOT OUTDIR)
  message(FATAL_ERROR "AGENTPDF, PDF, GOLD, OUTDIR required")
endif()

if(NOT HEUR)
  set(HEUR "${CMAKE_CURRENT_LIST_DIR}/../config/heuristics.json")
endif()

file(MAKE_DIRECTORY "${OUTDIR}")
get_filename_component(STEM "${PDF}" NAME_WE)
set(OUT_MD "${OUTDIR}/${STEM}.md")

execute_process(
  COMMAND "${AGENTPDF}" convert "${PDF}" -o "${OUTDIR}"
          --heuristics "${HEUR}"
          --metadata "${CMAKE_CURRENT_LIST_DIR}/../config/metadata.json"
  WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
  RESULT_VARIABLE RC
  OUTPUT_VARIABLE OUT
  ERROR_VARIABLE ERR
)
if(NOT RC EQUAL 0)
  message(FATAL_ERROR "agentpdf failed (${RC}): ${ERR}")
endif()

if(NOT EXISTS "${OUT_MD}")
  file(GLOB_RECURSE CANDIDATES "${OUTDIR}/${STEM}.md")
  list(LENGTH CANDIDATES N)
  if(N GREATER 0)
    list(GET CANDIDATES 0 OUT_MD)
  else()
    message(FATAL_ERROR "output markdown not found for ${STEM}")
  endif()
endif()

file(READ "${OUT_MD}" OUT_TEXT)
file(READ "${GOLD}" GOLD_TEXT)

function(strip_yaml text_var)
  set(t "${${text_var}}")
  string(FIND "${t}" "---" first)
  if(first EQUAL 0)
    string(SUBSTRING "${t}" 3 -1 rest)
    string(FIND "${rest}" "---" second)
    if(NOT second EQUAL -1)
      math(EXPR start "${second} + 3")
      string(SUBSTRING "${rest}" ${start} -1 t)
    endif()
  endif()
  set(${text_var} "${t}" PARENT_SCOPE)
endfunction()

strip_yaml(OUT_TEXT)
strip_yaml(GOLD_TEXT)

function(to_words text_var out_var)
  set(t "${${text_var}}")
  string(TOLOWER "${t}" t)
  string(REGEX REPLACE "[’‘]" "'" t "${t}")
  string(REGEX REPLACE "[“”]" "\"" t "${t}")
  string(REGEX REPLACE "([a-z0-9])- +([a-z0-9])" "\\1\\2" t "${t}")
  string(REGEX REPLACE "[^a-z0-9']+" " " t "${t}")
  string(STRIP "${t}" t)
  set(${out_var} "${t}" PARENT_SCOPE)
endfunction()

to_words(OUT_TEXT OUT_WORDS)
to_words(GOLD_TEXT GOLD_WORDS)

separate_arguments(OUT_LIST UNIX_COMMAND "${OUT_WORDS}")
separate_arguments(GOLD_LIST UNIX_COMMAND "${GOLD_WORDS}")

list(LENGTH OUT_LIST OUT_N)
list(LENGTH GOLD_LIST GOLD_N)

set(LIMIT ${GOLD_N})
if(OUT_N LESS GOLD_N)
  set(LIMIT ${OUT_N})
endif()

math(EXPR LAST "${LIMIT} - 1")
set(MISMATCH -1)
if(LAST GREATER_EQUAL 0)
  foreach(i RANGE 0 ${LAST})
    list(GET OUT_LIST ${i} ow)
    list(GET GOLD_LIST ${i} gw)
    if(NOT ow STREQUAL gw)
      set(MISMATCH ${i})
      break()
    endif()
  endforeach()
endif()

if(NOT MISMATCH EQUAL -1)
  math(EXPR START "${MISMATCH}")
  if(START GREATER 5)
    math(EXPR START "${MISMATCH} - 5")
  else()
    set(START 0)
  endif()
  math(EXPR END "${MISMATCH} + 5")
  set(CTX_OUT "")
  set(CTX_GOLD "")
  foreach(i RANGE ${START} ${END})
    if(i LESS OUT_N)
      list(GET OUT_LIST ${i} ow)
      string(APPEND CTX_OUT "${ow} ")
    endif()
    if(i LESS GOLD_N)
      list(GET GOLD_LIST ${i} gw)
      string(APPEND CTX_GOLD "${gw} ")
    endif()
  endforeach()
  message(FATAL_ERROR
    "word-order mismatch at index ${MISMATCH} (out ${OUT_N} words, gold ${GOLD_N} words)\n"
    "  out : ${CTX_OUT}\n"
    "  gold: ${CTX_GOLD}")
endif()

if(NOT OUT_N EQUAL GOLD_N)
  message(FATAL_ERROR "word count differs: out=${OUT_N} gold=${GOLD_N} (prefix matched)")
endif()

message(STATUS "word-order OK for ${STEM} (${OUT_N} words)")
