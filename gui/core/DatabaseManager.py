import sqlite3
import os
import datetime
import json

class DatabaseManager:
    def __init__(self, db_path):
        self.db_path = db_path
        self.init_db()

    def init_db(self):
        os.makedirs(os.path.dirname(self.db_path), exist_ok=True)
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        
        # 레거시 스키마 호환 유지
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS run_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                start_time TEXT,
                output_file TEXT,
                hv TEXT,
                config_dump TEXT
            )
        ''')
        
        # 🌟 제1원리: 기존 DB 파괴 없이 JSON 메타데이터 및 통계 컬럼 동적 추가
        cursor.execute("PRAGMA table_info(run_history)")
        columns = [info[1] for info in cursor.fetchall()]
        if 'env_metadata' not in columns:
            cursor.execute("ALTER TABLE run_history ADD COLUMN env_metadata TEXT")
        if 'daq_summary' not in columns:
            cursor.execute("ALTER TABLE run_history ADD COLUMN daq_summary TEXT")
        if 'production_summary' not in columns:
            cursor.execute("ALTER TABLE run_history ADD COLUMN production_summary TEXT")
            
        conn.commit()
        conn.close()

    def record_run_start(self, output_file, env_dict, config_path):
        config_dump = ""
        if os.path.exists(config_path):
            with open(config_path, 'r') as f: config_dump = f.read()
            
        env_json = json.dumps(env_dict, ensure_ascii=False)
        hv_fallback = env_dict.get("AppliedHV", "Unknown") 
        
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        start_time = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        
        cursor.execute('''
            INSERT INTO run_history (start_time, output_file, hv, env_metadata, config_dump)
            VALUES (?, ?, ?, ?, ?)
        ''', (start_time, output_file, hv_fallback, env_json, config_dump))
        run_id = cursor.lastrowid
        conn.commit()
        conn.close()
        return run_id

    def update_daq_summary(self, run_id, summary_dict):
        summary_json = json.dumps(summary_dict, ensure_ascii=False)
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute("UPDATE run_history SET daq_summary = ? WHERE id = ?", (summary_json, run_id))
        conn.commit()
        conn.close()

    def update_production_summary(self, raw_file_path, summary_dict):
        summary_json = json.dumps(summary_dict, ensure_ascii=False)
        conn = sqlite3.connect(self.db_path)
        cursor = conn.cursor()
        cursor.execute("UPDATE run_history SET production_summary = ? WHERE output_file = ?", (summary_json, raw_file_path))
        conn.commit()
        conn.close()
