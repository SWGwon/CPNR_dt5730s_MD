#include "EventHeader.h"
#include <TApplication.h>
#include <TCanvas.h>
#include <TFile.h>
#include <TGraph.h>
#include <TTree.h>
#include <TMacro.h>
#include <TParameter.h>
#include <TDatime.h>
#include <TSystem.h> // 비동기 디버거용 gSystem
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <csignal>
#include <numeric>
#include <sys/select.h> // 파이썬 GUI 통신용 논블로킹 I/O
#include <unistd.h>

#ifdef __ROOTCLING__
#pragma link C++ class std::vector<uint16_t>+;
#endif

// ==============================================================================
// [시스템] Graceful Shutdown을 위한 시그널 핸들러
// ==============================================================================
volatile std::sig_atomic_t g_running = 1;

void sig_handler(int) {
    std::cout << "\n\033[1;33m[Interrupt] Received stop signal. Saving ROOT file gracefully...\033[0m\n";
    g_running = 0;
}

// 주의: MAX_CH는 EventHeader.h에 정의(constexpr int MAX_CH = 8;)되어 있으므로 여기서 재정의하지 않습니다.

int main(int argc, char **argv) {
    std::string input_file = "";
    std::string output_file = "";
    std::string config_file = ""; 
    int debug_event_id = -1;
    int run_number = 0;
    bool save_waveform = false; 

    int opt;
    while ((opt = getopt(argc, argv, "i:o:c:r:d:w")) != -1) {
        switch (opt) {
            case 'i': input_file = optarg; break;
            case 'o': output_file = optarg; break;
            case 'c': config_file = optarg; break;
            case 'r': run_number = std::stoi(optarg); break;
            case 'd': debug_event_id = std::stoi(optarg); break;
            case 'w': save_waveform = true; break;
        }
    }

    if (input_file.empty() && optind < argc) input_file = argv[optind];
    if (input_file.empty()) {
        std::cerr << "Usage: " << argv[0] << " [input.dat] [-o output.root] [-c config.conf] [-r run_number] [-d event_id] [-w]\n";
        return 1;
    }

    // 출력 파일명 자동 생성
    if (output_file.empty() && debug_event_id < 0) {
        size_t last_dot = input_file.find_last_of(".");
        size_t last_slash = input_file.find_last_of("/\\");
        if (last_dot == std::string::npos || (last_slash != std::string::npos && last_dot < last_slash)) {
            output_file = input_file + "_prod.root";
        } else {
            output_file = input_file.substr(0, last_dot) + "_prod.root";
        }
    }

    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    // 고속 I/O를 위한 4MB 버퍼링
    std::ifstream ifs;
    std::vector<char> read_buffer(4 * 1024 * 1024);
    ifs.rdbuf()->pubsetbuf(read_buffer.data(), read_buffer.size());
    
    ifs.open(input_file, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "[Error] Cannot open input file: " << input_file << "\n";
        return 1;
    }

    ifs.seekg(0, std::ios::end);
    size_t total_bytes = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    size_t processed_bytes = 0;

    TApplication *app = nullptr;
    TCanvas *c1 = nullptr;
    if (debug_event_id >= 0) {
        app = new TApplication("App", &argc, argv);
        c1 = new TCanvas("c1", "Interactive Debugger", 1000, 600);
    }

    TFile *fOut = nullptr;
    TTree *tOut = nullptr;
    EventHeader header;
    
    uint32_t record_len_branch = 0; 
    std::vector<uint16_t> wave_ch[MAX_CH];
    double charge_ch[MAX_CH] = {0.0};
    double pulse_start_time_ch[MAX_CH] = {0.0}; 
    double baseline_ch[MAX_CH] = {0.0}; 

    if (debug_event_id < 0) {
        fOut = new TFile(output_file.c_str(), "RECREATE");
        
        if (!config_file.empty()) {
            std::ifstream cfs(config_file);
            if (cfs.is_open()) {
                TMacro config_macro(config_file.c_str());
                config_macro.Write("RunConfig");
            }
        }
        
        TParameter<int> p_run_num("RunNumber", run_number);
        p_run_num.Write();

        tOut = new TTree("phys_tree", "DT5730 Physics Data");
        tOut->Branch("EventID", &header.EventID, "EventID/i");
        tOut->Branch("SyncTime_TTT", &header.ExtendedTTT, "SyncTime_TTT/l");
        tOut->Branch("ChannelMask", &header.ChannelMask, "ChannelMask/s");
        tOut->Branch("RecordLength", &record_len_branch, "RecordLength/i"); 

        for (int i = 0; i < MAX_CH; ++i) {
            tOut->Branch(Form("Charge_CH%d", i), &charge_ch[i], Form("Charge_CH%d/D", i));
            tOut->Branch(Form("PulseStart_T0_CH%d", i), &pulse_start_time_ch[i], Form("PulseStart_T0_CH%d/D", i));
            tOut->Branch(Form("Baseline_CH%d", i), &baseline_ch[i], Form("Baseline_CH%d/D", i)); 
            
            if (save_waveform) {
                tOut->Branch(Form("Waveform_CH%d", i), &wave_ch[i]);
            }
        }
    }

    std::vector<uint16_t> raw_waveform_buffer;
    uint32_t current_event = 0;
    
    // =========================================================================
    // [추가] Offline Dead Time 및 Live Time 연산용 추적 변수 (물리적 제1원리)
    // =========================================================================
    bool is_first_event = true;
    uint64_t first_ttt = 0;
    uint64_t last_ttt = 0;
    uint64_t total_acquired_samples = 0;
    uint32_t prev_board_counter = 0;
    uint64_t lost_events = 0;

    auto start_time = std::chrono::steady_clock::now();
    
    std::cout << "\033[1;32m[Production] Starting Universal Conversion...\033[0m\n";

    while (g_running && ifs.read(reinterpret_cast<char *>(&header), sizeof(EventHeader))) {
        processed_bytes += sizeof(EventHeader);
        current_event++;
        record_len_branch = header.RecordLength; 

        // ---------------------------------------------------------------------
        // [추가] 8ns 해상도 TTT 및 진성 데드타임(보드가 수집 윈도우를 열어둔 시간) 누적
        // ---------------------------------------------------------------------
        if (is_first_event) {
            first_ttt = header.ExtendedTTT;
            prev_board_counter = header.BoardEventCounter;
            is_first_event = false;
        } else {
            uint32_t diff = (header.BoardEventCounter - prev_board_counter) & 0xFFFFFF;
            if (diff > 1) {
                lost_events += (diff - 1); // 진성 버퍼풀로 누락된 카운트 추적
            }
        }
        last_ttt = header.ExtendedTTT;
        prev_board_counter = header.BoardEventCounter;
        
        total_acquired_samples += header.RecordLength;
        // ---------------------------------------------------------------------

        int active_ch = 0;
        for (int i = 0; i < MAX_CH; ++i) {
            if ((header.ChannelMask >> i) & 1) active_ch++;
            wave_ch[i].clear();
            charge_ch[i] = 0.0;
            pulse_start_time_ch[i] = -1.0;
            baseline_ch[i] = 0.0;
        }

        size_t wave_len = header.RecordLength * active_ch;
        size_t wave_bytes_size = wave_len * sizeof(uint16_t);

        raw_waveform_buffer.resize(wave_len);
        ifs.read(reinterpret_cast<char *>(raw_waveform_buffer.data()), wave_bytes_size);
        processed_bytes += wave_bytes_size;

        int offset = 0;
        for (int ch = 0; ch < MAX_CH; ++ch) {
            if ((header.ChannelMask >> ch) & 1) {
                uint16_t* trace_ptr = raw_waveform_buffer.data() + offset;
                size_t trace_len = header.RecordLength;

                // 🌟 메타데이터 기반 동적 베이스라인 (25%, 최대 150 샘플 방어)
                if (trace_len > 0) {
                    size_t baseline_samples = std::min((size_t)150, (size_t)(trace_len * 0.25));
                    
                    double baseline = 0.0;
                    for(size_t i = 0; i < baseline_samples; ++i) {
                        baseline += trace_ptr[i];
                    }
                    baseline /= baseline_samples;
                    baseline_ch[ch] = baseline;

                    // 1. Charge 적분 (음극성)
                    double charge = 0.0;
                    for(size_t i = baseline_samples; i < trace_len; ++i) {
                        if (trace_ptr[i] < baseline) {
                            charge += (baseline - trace_ptr[i]);
                        }
                    }
                    charge_ch[ch] = (charge > 0) ? charge : 0.0;

                    // 2. Micro-Time (T0) 탐색 (Baseline - 30.0 트리거)
                    double trigger_threshold = baseline - 30.0; 
                    for(size_t i = baseline_samples; i < trace_len; ++i) {
                        if (trace_ptr[i] < trigger_threshold) {
                            pulse_start_time_ch[ch] = i * 2.0; // 2ns per sample
                            break;
                        }
                    }
                }

                if (save_waveform || (debug_event_id >= 0 && (int)header.EventID == debug_event_id)) {
                    wave_ch[ch].assign(trace_ptr, trace_ptr + trace_len);
                }
                offset += trace_len;
            }
        }

        if (tOut) tOut->Fill();

        // 정밀 ETA 및 스피드 진행률 표시
        if (current_event % 2000 == 0) {
            auto now = std::chrono::steady_clock::now();
            double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
            double progress = (static_cast<double>(processed_bytes) / total_bytes) * 100.0;
            double speed_bps = processed_bytes / elapsed_sec; 
            double eta_sec = (total_bytes - processed_bytes) / speed_bps;

            std::cout << "\r\033[K" << "[Progress] " 
                      << std::fixed << std::setprecision(1) << progress << "% | "
                      << "Events: " << current_event << " | "
                      << "Speed: " << std::setprecision(1) << (speed_bps / 1024.0 / 1024.0) << " MB/s | "
                      << "ETA: " << (int)eta_sec << " s" << std::flush;
        }

        // 🌟 인터랙티브 디버거 모드 (비동기 I/O 통신 적용)
        if (debug_event_id >= 0 && (int)header.EventID == debug_event_id && active_ch > 0) {
            int disp_ch = 0;
            for (; disp_ch < MAX_CH; ++disp_ch) {
                if ((header.ChannelMask >> disp_ch) & 1) break;
            }
            std::vector<double> x(header.RecordLength), y(header.RecordLength);
            for (size_t i = 0; i < header.RecordLength; ++i) {
                x[i] = i * 2.0; 
                y[i] = wave_ch[disp_ch][i];
            }
            TGraph *gr = new TGraph(header.RecordLength, x.data(), y.data());
            gr->SetTitle(Form("Event %d (CH%d) - Charge: %.1f, T0: %.1f ns;Time (ns);ADC Value", 
                              debug_event_id, disp_ch, charge_ch[disp_ch], pulse_start_time_ch[disp_ch]));
            gr->SetLineColor(kBlue);
            gr->SetLineWidth(2);
            gr->Draw("AL");

            TGraph* bl_line = new TGraph(2);
            bl_line->SetPoint(0, 0, baseline_ch[disp_ch]);
            bl_line->SetPoint(1, header.RecordLength * 2.0, baseline_ch[disp_ch]);
            bl_line->SetLineColor(kRed);
            bl_line->SetLineStyle(2);
            bl_line->SetLineWidth(2);
            bl_line->Draw("L SAME");

            c1->Update();
            
            std::cout << "\n\n\033[1;33m[Debugger] Displaying Event " << debug_event_id << " CH" << disp_ch << "\033[0m\n";
            std::cout << "RecordLength: " << header.RecordLength << " | Baseline: " << baseline_ch[disp_ch] << "\n";
            std::cout << "[WAITING_CMD] Ready for Python GUI Input (p/n/j/q)...\n";
            std::cout << std::flush; 

            std::string cmd;
            bool continue_debug = true;
            
            // 파이썬 GUI와 통신하기 위한 비동기 입력 루프
            while (continue_debug && g_running) {
                gSystem->ProcessEvents(); // GUI 창이 얼지 않도록 이벤트 처리

                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(STDIN_FILENO, &readfds);
                struct timeval timeout;
                timeout.tv_sec = 0;
                timeout.tv_usec = 100000; // 0.1초마다 폴링

                if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout) > 0) {
                    std::cin >> cmd;
                    if (cmd == "q" || cmd == "quit") {
                        std::cout << "\n[Debugger] Exiting debugger. Resuming full conversion...\n";
                        debug_event_id = -1; 
                        continue_debug = false;
                        if(c1) { c1->Close(); delete c1; c1 = nullptr; }
                    } 
                    else if (cmd == "n" || cmd == "next") {
                        debug_event_id++; 
                        std::cout << "\n[Debugger] Moving to next event (" << debug_event_id << ")...\n";
                        continue_debug = false;
                    } 
                    else if (cmd == "p" || cmd == "prev") {
                        std::cout << "\n[Debugger] 'prev' stream is forward only. Moving to next instead.\n";
                        debug_event_id++; 
                        continue_debug = false;
                    } 
                    else if (cmd == "j" || cmd == "jump") {
                        int target;
                        std::cin >> target;
                        if (target > (int)current_event) {
                            debug_event_id = target;
                            std::cout << "\n[Debugger] Jumping to event " << debug_event_id << "...\n";
                            continue_debug = false;
                        } else {
                            std::cout << "\n[Debugger] Target event (" << target << ") is already passed. Ignoring.\n";
                        }
                    }
                    std::cout << std::flush;
                }
            }
            if (debug_event_id >= 0) continue; // 다음 디버그 이벤트를 찾기 위해 진행
        }
    }

    if (g_running && debug_event_id < 0) {
        std::cout << "\r\033[K[Progress] 100.0% | Events: " << current_event << " | Done.          \n";

        // =====================================================================
        // [추가] 오프라인 변환 요약 출력 및 TParameter 영구 저장 (물리적 제1원리)
        // =====================================================================
        double real_time_sec = (last_ttt > first_ttt) ? (last_ttt - first_ttt) * 8e-9 : 0.0;
        double dead_time_sec = total_acquired_samples * 2e-9; // 1 Sample = 2ns
        double live_time_sec = real_time_sec - dead_time_sec;
        if (live_time_sec < 0) live_time_sec = 0.0;
        
        uint64_t total_triggers = current_event + lost_events;
        double lost_events_pct = (total_triggers > 0) ? (static_cast<double>(lost_events) / total_triggers * 100.0) : 0.0;
        double dead_time_pct = (real_time_sec > 0) ? (dead_time_sec / real_time_sec * 100.0) : 0.0;

        std::cout << "\n\033[1;36m========== [ ROOT Conversion Summary ] ==========\033[0m\n"
                  << " - Recorded Events : " << current_event << "\n"
                  << " - Lost Events     : " << lost_events << " (" << std::fixed << std::setprecision(3) << lost_events_pct << " %, Board Buffer Full)\n"
                  << " - HW Real Time    : " << std::fixed << std::setprecision(2) << real_time_sec << " sec\n"
                  << " - HW Live Time    : " << std::fixed << std::setprecision(2) << live_time_sec << " sec\n"
                  << " - True Dead Time  : " << std::fixed << std::setprecision(5) << dead_time_pct << " % (Record Window)\n"
                  << "\033[1;36m=================================================\033[0m\n\n";

        if (fOut) {
            fOut->cd();
            TParameter<double> p_real("RealTime_sec", real_time_sec);
            TParameter<double> p_live("LiveTime_sec", live_time_sec);
            TParameter<double> p_dead("DeadTime_pct", dead_time_pct);
            TParameter<int> p_lost("LostEvents_count", lost_events);
            TParameter<int> p_rec("RecordedEvents_count", current_event);
            
            p_real.Write();
            p_live.Write();
            p_dead.Write();
            p_lost.Write();
            p_rec.Write();
        }
    }

    if (fOut) {
        fOut->Write();
        fOut->Close();
        delete fOut;
        std::cout << "\033[1;32m[Production] Conversion complete. Saved to \033[0m" << output_file << "\n";
    }

    if (app) delete app;
    return 0;
}