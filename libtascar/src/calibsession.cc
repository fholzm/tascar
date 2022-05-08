/*
 * This file is part of the TASCAR software, see <http://tascar.org/>
 *
 * Copyright (c) 2022 Giso Grimm
 */
/**
 * @file calibsession.cc
 * @brief Speaker calibration class
 * @ingroup libtascar
 * @author Giso Grimm
 * @date 2022
 *
 * @section license License (GPL)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "calibsession.h"
#include "fft.h"
#include "jackiowav.h"
#include <unistd.h>

using namespace TASCAR;

std::string get_calibfor(const std::string& fname)
{
  TASCAR::xml_doc_t doc(fname, TASCAR::xml_doc_t::LOAD_FILE);
  return doc.root.get_attribute("calibfor");
}

std::vector<std::string> string_token(std::string s,
                                      const std::string& delimiter)
{
  std::vector<std::string> rv;
  size_t pos = 0;
  std::string token;
  while((pos = s.find(delimiter)) != std::string::npos) {
    token = s.substr(0, pos);
    rv.push_back(token);
    s.erase(0, pos + delimiter.length());
  }
  rv.push_back(s);
  return rv;
}

spk_eq_param_t::spk_eq_param_t(bool issub_) : issub(issub_)
{
  factory_reset();
}

void spk_eq_param_t::factory_reset()
{
  if(issub) {
    fmin = 31.25f;
    fmax = 62.5f;
    duration = 4.0f;
    prewait = 0.125f;
    reflevel = 70.0f;
    bandsperoctave = 3.0f;
    bandoverlap = 2.0f;
  } else {
    fmin = 62.5f;
    fmax = 4000.0f;
    duration = 1.0f;
    prewait = 0.125;
    reflevel = 70.0f;
    bandsperoctave = 3.0f;
    bandoverlap = 2.0f;
  }
}

#define READ_DEF(x) x = (float)TASCAR::config(path + "." #x, x)

void spk_eq_param_t::read_defaults()
{
  factory_reset();
  std::string path = "tascar.spkcalib";
  if(issub)
    path = "tascar.spkcalib.sub";
  READ_DEF(fmin);
  READ_DEF(fmax);
  READ_DEF(duration);
  READ_DEF(prewait);
  READ_DEF(reflevel);
  READ_DEF(bandsperoctave);
  READ_DEF(bandoverlap);
}

void spk_eq_param_t::read_xml(const tsccfg::node_t& layoutnode)
{
  TASCAR::xml_element_t xml(layoutnode);
  tsccfg::node_t spkcalibnode;
  if(issub)
    spkcalibnode = xml.find_or_add_child("subcalibconfig");
  else
    spkcalibnode = xml.find_or_add_child("speakercalibconfig");
  TASCAR::xml_element_t e(spkcalibnode);
  e.GET_ATTRIBUTE(fmin, "Hz", "Lower frequency limit of calibration.");
  e.GET_ATTRIBUTE(fmax, "Hz", "Upper frequency limit of calibration.");
  e.GET_ATTRIBUTE(duration, "s", "Stimulus duration.");
  e.GET_ATTRIBUTE(prewait, "s",
                  "Time between stimulus onset and measurement start.");
  e.GET_ATTRIBUTE(reflevel, "dB", "Reference level.");
  e.GET_ATTRIBUTE(bandsperoctave, "bpo",
                  "Bands per octave in filterbank for level equalization.");
  e.GET_ATTRIBUTE(
      bandoverlap, "bands",
      "Overlap in frequency bands in filterbank for level equalization.");
}

void spk_eq_param_t::save_xml(const tsccfg::node_t& layoutnode)
{
  TASCAR::xml_element_t xml(layoutnode);
  tsccfg::node_t spkcalibnode;
  if(issub)
    spkcalibnode = xml.find_or_add_child("subcalibconfig");
  else
    spkcalibnode = xml.find_or_add_child("speakercalibconfig");
  TASCAR::xml_element_t e(spkcalibnode);
  e.SET_ATTRIBUTE(fmin);
  e.SET_ATTRIBUTE(fmax);
  e.SET_ATTRIBUTE(duration);
  e.SET_ATTRIBUTE(prewait);
  e.SET_ATTRIBUTE(reflevel);
  e.SET_ATTRIBUTE(bandsperoctave);
  e.SET_ATTRIBUTE(bandoverlap);
}

void add_stimulus_plugin(xml_element_t node, const spk_eq_param_t& par)
{
  xml_element_t e_plugs(node.find_or_add_child("plugins"));
  xml_element_t e_pink(e_plugs.add_child("pink"));
  e_pink.set_attribute("level", TASCAR::to_string(par.reflevel));
  e_pink.set_attribute("period", TASCAR::to_string(par.duration));
  e_pink.set_attribute("fmin", TASCAR::to_string(par.fmin));
  e_pink.set_attribute("fmax", TASCAR::to_string(par.fmax));
}

calibsession_t::calibsession_t(const std::string& fname,
                               const std::vector<std::string>& refport,
                               const spk_eq_param_t& par_speaker_,
                               const spk_eq_param_t& par_sub_)
    : session_t("<?xml version=\"1.0\"?><session srv_port=\"none\"/>",
                LOAD_STRING, ""),
      gainmodified(false), levelsrecorded(false), calibrated(false),
      calibrated_diff(false), startlevel(0), startdiffgain(0), delta(0),
      delta_diff(0), spkname(fname), spk_file(NULL), par_speaker(par_speaker_),
      par_sub(par_sub_), refport_(refport), lmin(0), lmax(0), lmean(0),
      calibfor(get_calibfor(fname)), jackrec(refport_.size() + 1, "spkcalibrec")
{
  for(size_t ich = 0; ich < refport_.size() + 1; ++ich) {
    bbrecbuf.push_back((uint32_t)(jackrec.get_srate() * par_speaker.duration));
    subrecbuf.push_back((uint32_t)(jackrec.get_srate() * par_sub.duration));
  }
  if(calibfor.empty())
    calibfor = "type:nsp";
  // create a new session, no OSC port:
  root.set_attribute("srv_port", "none");
  // add the calibration scene:
  xml_element_t e_scene(root.add_child("scene"));
  e_scene.set_attribute("name", "calib");
  // add a point source for broadband stimulus, muted for now:
  xml_element_t e_src(e_scene.add_child("source"));
  e_src.set_attribute("mute", "true");
  // add pink noise generator:
  add_stimulus_plugin(e_src.add_child("sound"), par_speaker);
  // add a point source for subwoofer stimulus, muted for now:
  xml_element_t e_subsrc(e_scene.add_child("source"));
  e_subsrc.set_attribute("name", "srcsub");
  e_subsrc.set_attribute("mute", "true");
  add_stimulus_plugin(e_subsrc.add_child("sound"), par_sub);
  // receiver 1 is always nsp, for speaker level differences:
  xml_element_t e_rcvr(e_scene.add_child("receiver"));
  e_rcvr.set_attribute("type", "nsp");
  e_rcvr.set_attribute("layout", fname);
  // receiver 2 is specific to the layout, for overall calibration:
  xml_element_t e_rcvr2(e_scene.add_child("receiver"));
  e_rcvr2.set_attribute("name", "out2");
  e_rcvr2.set_attribute("mute", "true");
  e_rcvr2.set_attribute("layout", fname);
  // receiver 3 is omni, for reference signal:
  xml_element_t e_rcvr3(e_scene.add_child("receiver"));
  e_rcvr3.set_attribute("type", "omni");
  e_rcvr3.set_attribute("name", "ref");
  std::vector<std::string> receivertypeattr(string_token(calibfor, ","));
  for(auto typeattr : receivertypeattr) {
    std::vector<std::string> pair(string_token(typeattr, ":"));
    if(pair.size() != 2)
      throw TASCAR::ErrMsg(
          "Invalid format of 'calibfor' attribute '" + calibfor +
          "': Expected comma separated list of name:value pairs.");
    e_rcvr2.set_attribute(pair[0], pair[1]);
  }
  // add diffuse source for diffuse gain calibration:
  xml_element_t e_diff(e_scene.add_child("diffuse"));
  e_diff.set_attribute("mute", "true");
  add_stimulus_plugin(e_diff, par_speaker);
  // extra routes:
  xml_element_t e_mods(root.add_child("modules"));
  xml_element_t e_route_pink(e_mods.add_child("route"));
  e_route_pink.set_attribute("name", "pink");
  e_route_pink.set_attribute("channels", "1");
  add_stimulus_plugin(e_route_pink, par_speaker);
  xml_element_t e_route_sub(e_mods.add_child("route"));
  e_route_sub.set_attribute("name", "sub");
  e_route_sub.set_attribute("channels", "1");
  add_stimulus_plugin(e_route_sub, par_sub);
  // end of scene creation.
  // doc->write_to_file_formatted("temp.cfg");
  add_scene(e_scene.e);
  add_module(e_route_pink.e);
  add_module(e_route_sub.e);
  spk_file = new spk_array_diff_render_t(e_rcvr.e, false);
  levels = std::vector<float>(spk_file->size(), 0.0);
  sublevels = std::vector<float>(spk_file->subs.size(), 0.0);
  levelsfrg = std::vector<float>(spk_file->size(), 0.0);
  sublevelsfrg = std::vector<float>(spk_file->subs.size(), 0.0);
  // validate scene:
  if(scenes.empty())
    throw TASCAR::ErrMsg("Programming error: no scene");
  if(scenes[0]->source_objects.size() != 2)
    throw TASCAR::ErrMsg("Programming error: not exactly two sources.");
  if(scenes[0]->receivermod_objects.size() != 3)
    throw TASCAR::ErrMsg("Programming error: not exactly three receivers.");
  scenes.back()->source_objects[0]->dlocation = pos_t(1, 0, 0);
  // for(const auto& spk : *spk_file)
  //  max_fcomp_bb = std::max(max_fcomp_bb, spk.eqstages);
  // for(const auto& spk : spk_file->subs)
  //  max_fcomp_sub = std::max(max_fcomp_sub, spk.eqstages);
  rec_nsp = scenes.back()->receivermod_objects[0];
  spk_nsp = dynamic_cast<TASCAR::receivermod_base_speaker_t*>(rec_nsp->libdata);
  if(!spk_nsp)
    throw TASCAR::ErrMsg("Programming error: Invalid speaker type.");
  rec_spec = scenes.back()->receivermod_objects[1];
  spk_spec =
      dynamic_cast<TASCAR::receivermod_base_speaker_t*>(rec_spec->libdata);
  if(!spk_spec)
    throw TASCAR::ErrMsg("Programming error: Invalid speaker type.");
  startlevel = get_caliblevel();
  startdiffgain = get_diffusegain();
  for(auto recspk : {spk_nsp, spk_spec}) {
    for(auto& spk : recspk->spkpos)
      spk.eqstages = 0;
    for(auto& spk : recspk->spkpos.subs)
      spk.eqstages = 0;
  }
}

calibsession_t::~calibsession_t()
{
  delete spk_file;
}

void calibsession_t::reset_levels()
{
  levelsrecorded = false;
  for(auto& r : levelsfrg)
    r = 0.0f;
  for(auto& r : sublevelsfrg)
    r = 0.0f;
  for(auto recspk : {spk_nsp, spk_spec}) {
    for(uint32_t k = 0; k < levels.size(); ++k)
      recspk->spkpos[k].gain = 1.0;
    for(uint32_t k = 0; k < sublevels.size(); ++k)
      recspk->spkpos.subs[k].gain = 1.0;
  }
}

void get_levels_(spk_array_t& spks, TASCAR::Scene::src_object_t& src,
                 jackrec2wave_t& jackrec,
                 const std::vector<TASCAR::wave_t>& recbuf,
                 const std::vector<std::string>& ports,
                 levelmeter::weight_t weight, const spk_eq_param_t& calibpar,
                 std::vector<float>& levels, std::vector<float>& levelrange)
{
  levels.clear();
  levelrange.clear();
  std::vector<float> vF;
  std::vector<float> vL;
  // measure levels of all broadband speakers:
  for(auto& spk : spks) {
    // move source to speaker position:
    src.dlocation = spk.unitvector;
    usleep((unsigned int)(1e6f * calibpar.prewait));
    // record measurement signal:
    jackrec.rec(recbuf, ports);
    //
    TASCAR::levelmeter_t levelmeter((float)jackrec.get_srate(),
                                    calibpar.duration, weight);
    // calc average across input channels:
    float lev_sqr = 0.0f;
    std::vector<float> vLmean;
    std::vector<float> vLref;
    for(size_t ch = 0u; ch < ports.size() - 1u; ++ch) {
      auto& wav = recbuf[ch];
      levelmeter.update(wav);
      lev_sqr += levelmeter.ms();
      TASCAR::get_bandlevels(
          wav, calibpar.fmin, calibpar.fmax, (float)jackrec.get_srate(),
          calibpar.bandsperoctave, calibpar.bandoverlap, vF, vL);
      for(auto& l : vL)
        l = powf(10.0f, 0.1f * l);
      if(vLmean.empty())
        vLmean = vL;
      else {
        for(size_t k = 0; k < vL.size(); ++k)
          vLmean[k] += vL[k];
      }
    }
    for(auto& l : vLmean) {
      l /= (float)recbuf.size();
      l = 10.0f * log10f(l);
    }
    std::cout << "vLmeas2 = [" << TASCAR::to_string(vLmean) << "];\n";
    std::cout << "vLmeas1 = vLmeas1-vLref;\n";
    std::cout << "vLmeas2 = vLmeas2-vLref;\n";
    std::cout << "plot(vF,[vLmeas2-max(vLmeas2);vLmeas1-max(vLmeas1)],'-*');\n";
    std::cout << "set(gca,'XScale','log','XLim',[min(vF),max(vF)]);legend({'"
                 "post','pre'});\n";
    levelmeter.update(recbuf.back());
    TASCAR::get_bandlevels(recbuf.back(), calibpar.fmin, calibpar.fmax,
                           (float)jackrec.get_srate(), calibpar.bandsperoctave,
                           calibpar.bandoverlap, vF, vLref);
    lev_sqr /= (float)recbuf.size();
    lev_sqr = 10.0f * log10f(lev_sqr);
    levels.push_back(lev_sqr);
    for(size_t ch = 0; ch < std::min(vLmean.size(), vLref.size()); ++ch)
      vLmean[ch] = vLref[ch] - vLmean[ch];
    auto vl_min = vLmean.back();
    auto vl_max = vLmean.back();
    for(const auto& l : vLmean) {
      vl_min = std::min(vl_min, l);
      vl_max = std::max(vl_max, l);
    }
    levelrange.push_back(vl_max - vl_min);
  }
}

uint32_t get_fresp_(spk_array_t& spks, TASCAR::Scene::src_object_t& src,
                    jackrec2wave_t& jackrec,
                    const std::vector<TASCAR::wave_t>& recbuf,
                    const std::vector<std::string>& ports,
                    const spk_eq_param_t& calibpar, std::vector<float>& vF,
                    std::vector<std::vector<float>>& vGain)
{
  if(calibpar.max_eqstages == 0u)
    return 0u;
  vF.clear();
  vGain.clear();
  std::vector<float> vL;
  uint32_t numflt = 0u;
  // measure levels of all broadband speakers:
  for(auto& spk : spks) {
    // deactivate frequency correction:
    spk.eqstages = 0u;
    // move source to speaker position:
    src.dlocation = spk.unitvector;
    usleep((unsigned int)(1e6f * calibpar.prewait));
    // record measurement signal:
    jackrec.rec(recbuf, ports);
    // calc average across input channels:
    std::vector<float> vLmean;
    std::vector<float> vLref;
    for(size_t ch = 0u; ch < ports.size() - 1u; ++ch) {
      auto& wav = recbuf[ch];
      TASCAR::get_bandlevels(
          wav, calibpar.fmin, calibpar.fmax, (float)jackrec.get_srate(),
          calibpar.bandsperoctave, calibpar.bandoverlap, vF, vL);
      for(auto& l : vL)
        l = powf(10.0f, 0.1f * l);
      if(vLmean.empty())
        vLmean = vL;
      else {
        for(size_t k = 0; k < vL.size(); ++k)
          vLmean[k] += vL[k];
      }
    }
    for(auto& l : vLmean) {
      l /= (float)recbuf.size();
      l = 10.0f * log10f(l);
    }
    std::cout << "\n\nvF = [" << TASCAR::to_string(vF) << "];\n";
    std::cout << "vLmeas1 = [" << TASCAR::to_string(vLmean) << "];\n";
    TASCAR::get_bandlevels(recbuf.back(), calibpar.fmin, calibpar.fmax,
                           (float)jackrec.get_srate(), calibpar.bandsperoctave,
                           calibpar.bandoverlap, vF, vLref);
    std::cout << "vLref = [" << TASCAR::to_string(vLref) << "];\n";
    for(size_t ch = 0; ch < std::min(vLmean.size(), vLref.size()); ++ch)
      vLmean[ch] = vLref[ch] - vLmean[ch];
    auto vl_max = vLmean.back();
    for(const auto& l : vLmean)
      vl_max = std::max(vl_max, l);
    for(auto& l : vLmean)
      l -= vl_max;
    vGain.push_back(vLmean);
    if(numflt == 0)
      numflt = std::min(((uint32_t)vF.size() - 1u) / 3u, calibpar.max_eqstages);
    TASCAR::multiband_pareq_t eq;
    std::cout << "numflt = " << numflt << ";\n";
    eq.optim_response((size_t)numflt, vF, vLmean, (float)jackrec.get_srate());
    std::cout << eq.to_string();
    spk.eq = eq;
    spk.eqfreq = vF;
    spk.eqgain = vLmean;
    spk.eqstages = numflt;
  }
  return numflt;
}

void calibsession_t::get_levels()
{
  //
  // first broadband speakers:
  //
  auto allports = refport_;
  allports.push_back("render.calib:ref.0");
  // mute subwoofer source:
  scenes.back()->source_objects[1]->set_mute(true);
  // unmute broadband source:
  scenes.back()->source_objects[0]->set_mute(false);
  // unmute the NSP receiver:
  rec_spec->set_mute(true);
  rec_nsp->set_mute(false);
  fcomp_bb = get_fresp_(spk_nsp->spkpos, *(scenes.back()->source_objects[0]),
                        jackrec, bbrecbuf, allports, par_speaker, vF, vGains);
  // measure levels of all broadband speakers:
  get_levels_(spk_nsp->spkpos, *(scenes.back()->source_objects[0]), jackrec,
              bbrecbuf, allports, TASCAR::levelmeter::C, par_speaker, levels,
              levelsfrg);
  //
  // subwoofer:
  //
  if(!spk_nsp->spkpos.subs.empty()) {
    // mute broadband source:
    scenes.back()->source_objects[0]->set_mute(true);
    // unmute subwoofer source:
    scenes.back()->source_objects[1]->set_mute(false);
    fcomp_sub =
        get_fresp_(spk_nsp->spkpos.subs, *(scenes.back()->source_objects[1]),
                   jackrec, subrecbuf, allports, par_sub, vFsub, vGainsSub);
    get_levels_(spk_nsp->spkpos.subs, *(scenes.back()->source_objects[1]),
                jackrec, subrecbuf, allports, TASCAR::levelmeter::Z, par_sub,
                sublevels, sublevelsfrg);
  }
  // mute source and reset position:
  for(auto src : scenes.back()->source_objects) {
    src->set_mute(true);
    src->dlocation = pos_t(1, 0, 0);
  }
  // convert levels into gains:
  lmin = levels[0];
  lmax = levels[0];
  lmean = 0;
  for(auto l : levels) {
    lmean += l;
    lmin = std::min(l, lmin);
    lmax = std::max(l, lmax);
  }
  lmean /= (float)levels.size();
  // update gains of all receiver objects:
  for(auto recspk : {spk_nsp, spk_spec}) {
    // first modify gains:
    for(uint32_t k = 0; k < levels.size(); ++k)
      recspk->spkpos[k].gain *= pow(10.0, 0.05 * (lmin - levels[k]));
    for(uint32_t k = 0; k < sublevels.size(); ++k)
      recspk->spkpos.subs[k].gain *= pow(10.0, 0.05 * (lmin - sublevels[k]));
    // set max gain of broadband speakers to zero:
    double lmax(0);
    for(uint32_t k = 0; k < levels.size(); ++k)
      lmax = std::max(lmax, recspk->spkpos[k].gain);
    for(uint32_t k = 0; k < levels.size(); ++k)
      recspk->spkpos[k].gain /= lmax;
    for(uint32_t k = 0; k < sublevels.size(); ++k)
      recspk->spkpos.subs[k].gain /= lmax;
    {
      uint32_t k = 0;
      for(auto& spk : recspk->spkpos) {
        if(fcomp_bb) {
          spk.eq.optim_response((size_t)fcomp_bb, vF, vGains[k],
                                (float)jackrec.get_srate());
          spk.eqfreq = vF;
          spk.eqgain = vGains[k];
        } else {
          spk.eqfreq.clear();
          spk.eqgain.clear();
        }
        spk.eqstages = fcomp_bb;
        ++k;
      }
    }
    {
      uint32_t k = 0;
      for(auto& spk : recspk->spkpos.subs) {
        if(fcomp_sub) {
          spk.eq.optim_response((size_t)fcomp_sub, vFsub, vGainsSub[k],
                                (float)jackrec.get_srate());
          spk.eqfreq = vFsub;
          spk.eqgain = vGainsSub[k];
        } else {
          spk.eqfreq.clear();
          spk.eqgain.clear();
        }
        spk.eqstages = fcomp_sub;
      }
    }
  }
  levelsrecorded = true;
}

void calibsession_t::saveas(const std::string& fname)
{
  // convert levels into gains:
  std::vector<double> gains;
  float lmin(levels[0]);
  for(auto l : levels)
    lmin = std::min(l, lmin);
  for(uint32_t k = 0; k < levels.size(); ++k)
    gains.push_back(20 * log10((*spk_file)[k].gain) + lmin - levels[k]);
  // rewrite file:
  TASCAR::xml_doc_t doc(spkname, TASCAR::xml_doc_t::LOAD_FILE);
  if(doc.root.get_element_name() != "layout")
    throw TASCAR::ErrMsg(
        "Invalid file type, expected root node type \"layout\", got \"" +
        doc.root.get_element_name() + "\".");
  TASCAR::xml_element_t elayout(doc.root);
  elayout.set_attribute("caliblevel", TASCAR::to_string(get_caliblevel()));
  elayout.set_attribute("diffusegain", TASCAR::to_string(get_diffusegain()));
  // update gains:
  TASCAR::spk_array_diff_render_t array(doc.root(), true);
  size_t k = 0;
  for(auto spk : doc.root.get_children("speaker")) {
    xml_element_t espk(spk);
    auto& tscspk = spk_spec->spkpos[std::min(k, spk_spec->spkpos.size() - 1)];
    espk.set_attribute("gain", TASCAR::to_string(20 * log10(tscspk.gain)));
    espk.set_attribute("eqstages", std::to_string(fcomp_bb));
    if((fcomp_bb > 0) && (k < vGains.size())) {
      espk.set_attribute("eqfreq", TASCAR::to_string(vF));
      espk.set_attribute("eqgain", TASCAR::to_string(vGains[k]));
    } else {
      espk.set_attribute("eqfreq", "");
      espk.set_attribute("eqgain", "");
    }
    ++k;
  }
  k = 0;
  for(auto spk : doc.root.get_children("sub")) {
    xml_element_t espk(spk);
    auto& tscspk =
        spk_spec->spkpos.subs[std::min(k, spk_spec->spkpos.subs.size() - 1)];
    espk.set_attribute("gain", TASCAR::to_string(20 * log10(tscspk.gain)));
    espk.set_attribute("eqstages", std::to_string(fcomp_sub));
    if((fcomp_sub > 0) && (k < vGainsSub.size())) {
      espk.set_attribute("eqfreq", TASCAR::to_string(vFsub));
      espk.set_attribute("eqgain", TASCAR::to_string(vGainsSub[k]));
    } else {
      espk.set_attribute("eqfreq", "");
      espk.set_attribute("eqgain", "");
    }
    ++k;
  }
  size_t checksum = get_spklayout_checksum(elayout);
  elayout.set_attribute("checksum", (uint64_t)checksum);
  char ctmp[1024];
  memset(ctmp, 0, 1024);
  std::time_t t(std::time(nullptr));
  std::strftime(ctmp, 1023, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  doc.root.set_attribute("calibdate", ctmp);
  doc.root.set_attribute("calibfor", calibfor);
  par_speaker.save_xml(doc.root());
  par_sub.save_xml(doc.root());
  doc.save(fname);
  gainmodified = false;
  levelsrecorded = false;
  calibrated = false;
  calibrated_diff = false;
}

void calibsession_t::save()
{
  saveas(spkname);
}

void calibsession_t::set_active(bool b)
{
  // activate broadband source and type-specific receiver.
  scenes.back()->source_objects[1]->set_mute(true);
  if(!b) {
    // inactive broadband, so enable nsp receiver:
    rec_nsp->set_mute(false);
    rec_spec->set_mute(true);
  }
  if(b)
    // active, so mute diffuse sound:
    set_active_diff(false);
  scenes.back()->source_objects[0]->dlocation = pos_t(1, 0, 0);
  // activate broadband source if needed:
  scenes.back()->source_objects[0]->set_mute(!b);
  if(b) {
    // enable saving of file:
    calibrated = true;
    // active, so activate type-specific receiver:
    rec_nsp->set_mute(true);
    rec_spec->set_mute(false);
  }
}

void calibsession_t::set_active_diff(bool b)
{
  // control diffuse source:
  scenes.back()->source_objects[1]->set_mute(true);
  if(!b) {
    rec_nsp->set_mute(false);
    rec_spec->set_mute(true);
  }
  if(b)
    set_active(false);
  scenes.back()->diff_snd_field_objects.back()->set_mute(!b);
  if(b) {
    calibrated_diff = true;
    rec_nsp->set_mute(true);
    rec_spec->set_mute(false);
  }
}

double calibsession_t::get_caliblevel() const
{
  return 20.0 * log10(rec_spec->caliblevel * 5e4);
}

double calibsession_t::get_diffusegain() const
{
  return 20.0 * log10(rec_spec->diffusegain);
}

void calibsession_t::inc_caliblevel(double dl)
{
  gainmodified = true;
  delta += dl;
  double newlevel_pa(2e-5 * pow(10.0, 0.05 * (startlevel + delta)));
  rec_nsp->caliblevel = (float)newlevel_pa;
  rec_spec->caliblevel = (float)newlevel_pa;
}

void calibsession_t::inc_diffusegain(double dl)
{
  gainmodified = true;
  delta_diff += dl;
  double gain(pow(10.0, 0.05 * (startdiffgain + delta_diff)));
  rec_nsp->diffusegain = (float)gain;
  rec_spec->diffusegain = (float)gain;
}

/*
 * Local Variables:
 * mode: c++
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * compile-command: "make -C .."
 * End:
 */
