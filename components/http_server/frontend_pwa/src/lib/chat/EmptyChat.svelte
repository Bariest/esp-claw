<script>
  import { colors } from "../colors.js";

  const YELLOW = colors.mpx.primary;
</script>

<div class="empty-chat">
  <!-- Center content -->
  <div class="center-area">
    <!-- Eye-open icon -->
    <div class="dog-icon" style="background: {YELLOW}">
      <svg width="62" height="57" viewBox="0 0 62 57" fill="none" xmlns="http://www.w3.org/2000/svg">
        <rect width="62" height="57" rx="8" fill="#FFE605"/>
        <rect x="34.3752" y="18.5" width="12.7892" height="21.982" rx="2.5" fill="black"/>
        <rect x="34.3752" y="18.5" width="12.7892" height="21.982" rx="2.5" fill="black"/>
        <rect x="34.3752" y="18.5" width="12.7892" height="21.982" rx="2.5" fill="black"/>
        <rect x="34.3752" y="18.5" width="12.7892" height="21.982" rx="2.5" stroke="black"/>
        <rect x="13.5" y="18.5" width="12.7892" height="21.982" rx="3.5" fill="black"/>
        <rect x="13.5" y="18.5" width="12.7892" height="21.982" rx="3.5" fill="black"/>
        <rect x="13.5" y="18.5" width="12.7892" height="21.982" rx="3.5" fill="black"/>
        <rect x="13.5" y="18.5" width="12.7892" height="21.982" rx="3.5" stroke="black"/>
        <path d="M16.16 39.1627C15.2808 39.9071 15.1507 40.4867 14.7236 41.5566H26.0231C25.4466 40.5073 25.1009 39.9555 24.2037 39.1627C23.5586 38.5926 23.1864 38.2307 22.3843 37.9178C21.0953 37.415 19.5992 37.545 18.2667 37.9178C17.2519 38.2018 16.9642 38.4818 16.16 39.1627Z" fill="#FCE505" stroke="#FFE605"/>
        <path d="M36.4608 39.1097C35.5816 39.8541 35.4515 40.4337 35.0244 41.5037H46.3239C45.7474 40.4543 45.4017 39.9025 44.5045 39.1097C43.8594 38.5396 43.4871 38.1777 42.6851 37.8648C41.396 37.362 39.8999 37.492 38.5675 37.8648C37.5527 38.1488 37.265 38.4288 36.4608 39.1097Z" fill="#FFE605" stroke="#FFE605"/>
      </svg>
    </div>

    

    <!-- Tagline -->
    <h2 class="tagline">Mini Pupper is ready to talk</h2>
    <p class="subtitle">Ask me anything — I can help find deals,<br/>plan meals, or control your robot!</p>
  </div>
</div>

<style>
  .empty-chat {
    display: flex;
    flex-direction: column;
    height: 100%;
    background: #f8f8f8;
  }

  .center-area {
    flex: 1;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 0 24px;
    gap: 12px;
  }

  .dog-icon {
    width: 70px;
    height: 70px;
    border-radius: 16px;
    display: flex;
    align-items: center;
    justify-content: center;
    box-shadow: 0 4px 12px rgba(0,0,0,0.08);
  }

  .tagline {
    font-size: 1.05rem;
    font-weight: 700;
    color: #333;
    text-align: center;
    margin-top: 8px;
  }

  .subtitle {
    font-size: 0.8rem;
    color: #999;
    text-align: center;
    line-height: 1.5;
  }
</style>
