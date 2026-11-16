################################################################################
#
# Driver for the GPlotDesignerRootValidity ctest. It confirms that the ROOT macro
# GPlotDesigner generates is VALID ROOT input by feeding it to the real ROOT
# interpreter (headless, via xvfb-run) and checking the rendered artifact.
#
# ROOT must touch a display even in batch mode on this host, hence xvfb-run; and its
# headless teardown returns a non-zero exit code AFTER a successful render, so this
# driver validates the produced PDF rather than ROOT's exit status.
#
# Inputs (passed via -D): GEN_EXE, ROOT_EXE, XVFB_RUN, WORKDIR.
#
################################################################################

# 1. Run the generator -> WORKDIR/result.C
execute_process(
	COMMAND "${GEN_EXE}"
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE gen_rc
)
if(NOT gen_rc EQUAL 0)
	message(FATAL_ERROR "GPlotDesignerTest generator failed (rc=${gen_rc})")
endif()
if(NOT EXISTS "${WORKDIR}/result.C")
	message(FATAL_ERROR "generator did not produce result.C in ${WORKDIR}")
endif()

# 2. Build a probe macro: the generated macro with a SaveAs + sentinel inserted before
#    its final closing brace (so a successful render leaves a verifiable artifact). The probe also
#    reports, for every sub-pad of the graph pad in order, the legend ROOT drew: the number of its
#    entries ("GPD-PAD-LEGEND n", or "none"), and an entry with a blank text, without an object or
#    with the object of an earlier entry ("GPD-LEGEND-BAD ..."). Step 5 compares the numbers with
#    the description the macro states per pad ("// legend entries: N", written from
#    GBasePlotter::legendEntries()). For a legend placed in a corner (a fixed text size) it reports
#    whether each label fits into the box.
file(READ "${WORKDIR}/result.C" _macro)
string(FIND "${_macro}" "}" _last_brace REVERSE)
if(_last_brace LESS 0)
	message(FATAL_ERROR "result.C has no closing brace -- unexpected generator output")
endif()
string(SUBSTRING "${_macro}" 0 ${_last_brace} _head)
set(_legend_probe [=[
   {
      TIter l1(cc->GetListOfPrimitives());
      while(TObject *o1 = l1()) {
         TPad *p1 = dynamic_cast<TPad *>(o1); if(!p1) continue;
         TIter l2(p1->GetListOfPrimitives());
         while(TObject *o2 = l2()) {
            TPad *p2 = dynamic_cast<TPad *>(o2); if(!p2) continue;
            TLegend *l = nullptr;
            TIter l3(p2->GetListOfPrimitives());
            while(TObject *o3 = l3()) {
               if(TLegend *candidate = dynamic_cast<TLegend *>(o3)) l = candidate;
            }
            if(!l) { printf("GPD-PAD-LEGEND none\n"); continue; }
            printf("GPD-PAD-LEGEND %d\n", l->GetListOfPrimitives()->GetSize());
            TList seen;
            TIter l4(l->GetListOfPrimitives());
            while(TObject *o4 = l4()) {
               TLegendEntry *e = static_cast<TLegendEntry *>(o4);
               if(TString(e->GetLabel()).Strip(TString::kBoth).IsNull()) printf("GPD-LEGEND-BAD blank text\n");
               if(!e->GetObject()) printf("GPD-LEGEND-BAD no object for %s\n", e->GetLabel());
               else if(seen.FindObject(e->GetObject())) printf("GPD-LEGEND-BAD %s repeats an object\n", e->GetLabel());
               else seen.Add(e->GetObject());
            }
            if(l->GetTextSize() <= 0.) continue; // ROOT sizes the text to the box itself
            p2->cd();
            const double room = (l->GetX2NDC() - l->GetX1NDC()) * (1. - l->GetMargin());
            TIter l5(l->GetListOfPrimitives());
            while(TObject *o5 = l5()) {
               TLatex label(0., 0., static_cast<TLegendEntry *>(o5)->GetLabel());
               label.SetTextFont(l->GetTextFont());
               label.SetTextSize(l->GetTextSize());
               const double width = label.GetXsize() / (p2->GetX2() - p2->GetX1());
               printf("GPD-PLACED-LABEL %s\n", width > room ? "overflows" : "fits");
            }
         }
      }
   }
]=])
file(WRITE "${WORKDIR}/gpd_validate.C"
	"${_head}   cc->SaveAs(\"gpd_validated.pdf\");\n${_legend_probe}   printf(\"GPD-ROOT-VALID\\n\");\n}\n")
file(REMOVE "${WORKDIR}/gpd_validated.pdf")

# 3. Run it through ROOT headless.
execute_process(
	COMMAND "${XVFB_RUN}" -a "${ROOT_EXE}" -l -b -q gpd_validate.C
	WORKING_DIRECTORY "${WORKDIR}"
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
	TIMEOUT 300
)

# 4. Validate via the rendered artifact (NOT ROOT's exit code -- see header note).
if(NOT EXISTS "${WORKDIR}/gpd_validated.pdf")
	message(FATAL_ERROR
		"ROOT did not render the generated macro -- it is not valid ROOT input.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
file(SIZE "${WORKDIR}/gpd_validated.pdf" _sz)
if(_sz LESS 1000)
	message(FATAL_ERROR "ROOT produced a suspiciously small PDF (${_sz} bytes); the macro likely did not render.")
endif()

# 5. Every pad shows the legend its description states: as many entries as the macro says
#    ("// legend entries: N", one line per pad in the order of the pads), none where it says 0,
#    and no entry with a blank text, without an object or repeating one.
string(REGEX MATCHALL "// legend entries: [0-9]+" _expected "${_macro}")
string(REGEX MATCHALL "GPD-PAD-LEGEND [a-z0-9]+" _drawn "${_out}")
list(LENGTH _expected _n_expected)
list(LENGTH _drawn _n_drawn)
if(_n_expected EQUAL 0 OR _n_drawn LESS _n_expected)
	message(FATAL_ERROR "${_n_expected} pads described, ${_n_drawn} pads drawn.\nstdout:\n${_out}")
endif()
set(_n_legends 0)
foreach(_i RANGE 0 ${_n_drawn})
	if(_i EQUAL _n_drawn)
		break()
	endif()
	list(GET _drawn ${_i} _got)
	string(REGEX REPLACE "GPD-PAD-LEGEND " "" _got "${_got}")
	set(_want "none")
	if(_i LESS _n_expected)
		list(GET _expected ${_i} _want)
		string(REGEX REPLACE "// legend entries: " "" _want "${_want}")
		if(_want EQUAL 0)
			set(_want "none")
		endif()
	endif()
	if(NOT _got STREQUAL _want)
		message(FATAL_ERROR "Pad ${_i}: the description gives its legend ${_want} entries, ROOT drew ${_got}.\nstdout:\n${_out}")
	endif()
	if(NOT _got STREQUAL "none")
		math(EXPR _n_legends "${_n_legends} + 1")
	endif()
endforeach()
if(_n_legends EQUAL 0)
	message(FATAL_ERROR "No pad of the generator has a legend; the check proves nothing.\nstdout:\n${_out}")
endif()
if(_out MATCHES "GPD-LEGEND-BAD")
	message(FATAL_ERROR "A legend has a bad entry.\nstdout:\n${_out}")
endif()
# A placed legend has a fixed text size; a label too long for its box at that size must be drawn
# smaller rather than run out of the box over the pad.
string(REGEX MATCHALL "GPD-PLACED-LABEL [a-z]+" _placed "${_out}")
if(NOT _placed)
	message(FATAL_ERROR "ROOT reported no label of a placed legend, but the generated macro has one.\nstdout:\n${_out}")
endif()
if("GPD-PLACED-LABEL overflows" IN_LIST _placed)
	message(FATAL_ERROR "A label of a placed legend runs out of its box.\nstdout:\n${_out}")
endif()
message(STATUS "GPlotDesignerRootValidity: ROOT rendered a ${_sz}-byte PDF from the generated macro; "
	"${_n_legends} legends, each as its description states.")
