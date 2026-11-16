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
               if(!e->GetObject() && TString(e->GetOption()).Length() > 0) printf("GPD-LEGEND-BAD no object for %s\n", e->GetLabel());
               else if(!e->GetObject()) {} // a note without a sample (a crowded band's last row)
               else if(seen.FindObject(e->GetObject())) printf("GPD-LEGEND-BAD %s repeats an object\n", e->GetLabel());
               else seen.Add(e->GetObject());
               if(e->GetObject()) printf("GPD-ENTRY %s %s\n", e->GetOption(), e->GetObject()->GetName());
            }
            // A legend that grew for a long label stays within the frame's width, left of a palette.
            if(l->GetX2NDC() > 1. - p2->GetRightMargin() + 1e-9) printf("GPD-LEGEND-OUTSIDE %g\n", l->GetX2NDC());
            { // the points of the pad's graphs under the legend's box (and along their lines)
               p2->cd();
               const double ux1 = p2->GetX1() + l->GetX1NDC() * (p2->GetX2() - p2->GetX1());
               const double ux2 = p2->GetX1() + l->GetX2NDC() * (p2->GetX2() - p2->GetX1());
               const double uy1 = p2->GetY1() + l->GetY1NDC() * (p2->GetY2() - p2->GetY1());
               const double uy2 = p2->GetY1() + l->GetY2NDC() * (p2->GetY2() - p2->GetY1());
               int covered = 0;
               TIter l6(p2->GetListOfPrimitives());
               while(TObject *o6 = l6()) {
                  TGraph *g = dynamic_cast<TGraph *>(o6); if(!g || p2->GetView()) continue;
                  for(int i = 0; i < g->GetN(); ++i) {
                     for(int k = 0; k <= (i + 1 < g->GetN() ? 20 : 0); ++k) { // the point, and along to the next
                        double x = g->GetX()[i], y = g->GetY()[i];
                        if(k > 0) { const double t = k / 20.; x += t * (g->GetX()[i + 1] - x); y += t * (g->GetY()[i + 1] - y); }
                        if(p2->GetLogx()) x = TMath::Log10(x);
                        if(p2->GetLogy()) y = TMath::Log10(y);
                        if(x >= ux1 && x <= ux2 && y >= uy1 && y <= uy2) ++covered;
                     }
                  }
               }
               printf("GPD-PAD-COVERED %d\n", covered);
            }
            if(l->GetTextSize() <= 0.) continue; // ROOT sizes the text to the box itself
            // The floor of a placed legend's text: 0.035, and 0.025 in the crowded band of a 3-d pad
            // (six or seven rows; the band alone starts at the pad's left edge).
            const double floor_size = (p2->GetView() && l->GetX1NDC() < 0.05 && l->GetListOfPrimitives()->GetSize() > 5) ? 0.025 : 0.035;
            if(l->GetTextSize() < floor_size - 1e-9) printf("GPD-PLACED-SMALL %g\n", l->GetTextSize());
            if(p2->GetView() && l->GetListOfPrimitives()->GetSize() > 5) printf("GPD-CROWDED-BAND %d\n", l->GetListOfPrimitives()->GetSize());
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
# Every entry has the option that draws the sample the description gives it (a line "l", markers
# "p", both "lp", a fill or a colour map "f"), and the object it names, in order.
string(REGEX MATCHALL "// legend entry: [a-z_]+ [A-Za-z0-9_]+" _entries_wanted "${_macro}")
string(REGEX REPLACE "// legend entry: " "" _entries_wanted "${_entries_wanted}")
string(REGEX REPLACE "(^|;)line_and_marker " "\\1lp " _entries_wanted "${_entries_wanted}")
string(REGEX REPLACE "(^|;)line " "\\1l " _entries_wanted "${_entries_wanted}")
string(REGEX REPLACE "(^|;)marker " "\\1p " _entries_wanted "${_entries_wanted}")
string(REGEX REPLACE "(^|;)(fill|colour_map) " "\\1f " _entries_wanted "${_entries_wanted}")
string(REGEX MATCHALL "GPD-ENTRY [a-z]+ [A-Za-z0-9_]+" _entries_drawn "${_out}")
string(REGEX REPLACE "GPD-ENTRY " "" _entries_drawn "${_entries_drawn}")
if(NOT _entries_wanted)
	message(FATAL_ERROR "The macro states no legend entry; the check proves nothing.")
endif()
if(NOT _entries_wanted STREQUAL _entries_drawn)
	message(FATAL_ERROR "The legend entries ROOT drew are not the described ones.\ndescribed: ${_entries_wanted}\ndrawn:     ${_entries_drawn}")
endif()
if(_out MATCHES "GPD-LEGEND-OUTSIDE")
	message(FATAL_ERROR "A legend reaches beyond the pad's frame (over a palette).\nstdout:\n${_out}")
endif()
if(_out MATCHES "GPD-LEGEND-BAD")
	message(FATAL_ERROR "A legend has a bad entry.\nstdout:\n${_out}")
endif()
# A pad whose frame leaves its legend a band of its own ("// legend room: 1") has no data under the
# legend: no point and no stretch of a line (the probe reports one count per legend, in the order
# of the pads; the room comment stands in every pad).
string(REGEX MATCHALL "// legend room: [01]" _rooms "${_macro}")
string(REGEX MATCHALL "// legend entries: [0-9]+" _entry_counts "${_macro}")
string(REGEX MATCHALL "GPD-PAD-COVERED [0-9]+" _covered "${_out}")
set(_legend_index 0)
set(_checked_rooms 0)
list(LENGTH _rooms _n_rooms)
if(_n_rooms GREATER 0)
	math(EXPR _last_room "${_n_rooms} - 1")
	foreach(_i RANGE 0 ${_last_room})
		list(GET _entry_counts ${_i} _count)
		string(REGEX REPLACE "// legend entries: " "" _count "${_count}")
		if(_count EQUAL 0)
			continue()
		endif()
		list(GET _rooms ${_i} _room)
		list(GET _covered ${_legend_index} _cov)
		math(EXPR _legend_index "${_legend_index} + 1")
		if(_room STREQUAL "// legend room: 1")
			math(EXPR _checked_rooms "${_checked_rooms} + 1")
			if(NOT _cov STREQUAL "GPD-PAD-COVERED 0")
				message(FATAL_ERROR "Pad ${_i} leaves its legend a band of its own, yet ${_cov} samples of its data lie under the legend.\nstdout:\n${_out}")
			endif()
		endif()
	endforeach()
endif()
if(_checked_rooms EQUAL 0)
	message(FATAL_ERROR "No pad leaves its legend a band of its own; the check proves nothing.")
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
# ... but no smaller than ROOT's default size of axis labels (LEGENDMINTEXTSIZE): a label that
# does not fit at that size widens the box.
if(NOT _out MATCHES "GPD-CROWDED-BAND 7")
	message(FATAL_ERROR "The generator's crowded 3-d band was not drawn with six series and a summing row.\nstdout:\n${_out}")
endif()
if(NOT _macro MATCHES "and 3 more series")
	message(FATAL_ERROR "The macro has no summing row for the crowded band.\nstdout:\n${_out}")
endif()
if(_out MATCHES "GPD-PLACED-SMALL")
	message(FATAL_ERROR "A placed legend's text is smaller than the readable floor.\nstdout:\n${_out}")
endif()
message(STATUS "GPlotDesignerRootValidity: ROOT rendered a ${_sz}-byte PDF from the generated macro; "
	"${_n_legends} legends, each as its description states.")
